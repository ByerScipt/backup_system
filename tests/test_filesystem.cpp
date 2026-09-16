#include "helpers.hpp"

// A failed directory increment can also become the end iterator. Success must
// mean the whole tree was scanned, not just the entries preceding that error.
void testUnreadableSource()
{
    if (::geteuid() == 0)
    {
        std::cout << "SKIP: unreadable source requires a non-root user\n";
        return;
    }
    TempDirectory temp;
    const auto source = temp.path / "source";
    const auto blocked = source / "blocked";
    fs::create_directories(blocked);
    writeBytes(blocked / "important.txt", {'k', 'e', 'e', 'p'});
    check(::chmod(blocked.c_str(), 0000) == 0,
          "cannot restrict source fixture");
    const auto archive = temp.path / "incomplete.bak";
    const auto result =
        BackupEngine::create(source.string(), archive.string(), {});
    check(::chmod(blocked.c_str(), 0700) == 0, "cannot unlock source fixture");
    check(!result.success, "unreadable subtree produced a successful backup");
    check(result.message.find("enumerate") != std::string::npos,
          "unreadable subtree did not explain the scan failure");
    check(!fs::exists(archive), "failed scan published an incomplete archive");
}

void testMetadataFailure()
{
    if (::geteuid() == 0)
    {
        std::cout << "SKIP: ownership failure requires a non-root user\n";
        return;
    }
    TempDirectory temp;
    const auto source = temp.path / "source";
    fs::create_directory(source);
    writeBytes(source / "owned.txt", {'d', 'a', 't', 'a'});
    const auto archive = temp.path / "owner.bak";
    check(BackupEngine::create(source.string(), archive.string(), {}).success,
          "cannot create ownership fixture");
    auto bytes = readBytes(archive);
    const std::string path = "source/owned.txt";
    const auto found =
        std::search(bytes.begin() + 112, bytes.end(), path.begin(), path.end());
    check(found != bytes.end(), "cannot locate ownership fixture");
    // BKP metadata: path bytes, type (1), mode (4), uid (4), gid (4).
    const auto uid = found + path.size() + 5;
    std::fill(uid, uid + 4, 0xff);
    *uid = 0xfe; // A different uid, not chown's special unchanged sentinel.
    refreshPayloadDigests(bytes, true);
    writeBytes(archive, bytes);
    const auto destination = temp.path / "restored";
    const auto result =
        BackupEngine::restore(archive.string(), destination.string(), {});
    check(!result.success, "unrestored ownership was reported as successful");
    check(result.message.find("ownership") != std::string::npos,
          "ownership failure was not explained");
    check(!fs::exists(destination / path),
          "metadata failure committed the incomplete file");
}

void testHardLinks()
{
    TempDirectory temp;
    const auto source = temp.path / "source";
    fs::create_directories(source / "nested");
    // Version 1 had the same ordinary-entry layout. Keep that reader covered.
    const auto oldArchive = temp.path / "legacy.bak";
    check(
        BackupEngine::create(source.string(), oldArchive.string(), {}).success,
        "cannot create legacy layout fixture");
    auto oldBytes = readBytes(oldArchive);
    oldBytes[4] = 1;
    writeBytes(oldArchive, oldBytes);
    check(BackupEngine::restore(oldArchive.string(),
                                (temp.path / "legacy").string(), {})
              .success,
          "version 1 directory archive is no longer readable");
    writeBytes(source / "original", {'h', 'a', 'r', 'd'});
    check(::link((source / "original").c_str(),
                 (source / "nested" / "alias").c_str()) == 0,
          "cannot create hardlink fixture");
    bool accessed = false;
    BackupOptions accessDuringScan;
    accessDuringScan.progress = [&](const ProgressEvent& event)
    {
        if (!accessed && event.stage == "scan" &&
            (event.detail == "source/original" ||
             event.detail == "source/nested/alias"))
        {
            timespec times[2]{{123456789, 42}, {0, UTIME_OMIT}};
            check(::utimensat(AT_FDCWD, (source / "original").c_str(), times,
                              0) == 0,
                  "cannot change source access time");
            accessed = true;
        }
    };
    const auto accessedArchive = temp.path / "accessed.bak";
    check(BackupEngine::create(source.string(), accessedArchive.string(),
                               accessDuringScan)
              .success,
          "access-time-only change rejected backup");
    check(accessed &&
              BackupEngine::restore(accessedArchive.string(),
                                    (temp.path / "accessed").string(), {})
                  .success,
          "access-time change produced an unreadable hardlink archive");
    // The canonical archive name is selected by sorted path, not scan order.
    for (auto pack : {PackAlgorithm::Stream, PackAlgorithm::Index})
    {
        for (auto compression :
             {CompressionAlgorithm::None, CompressionAlgorithm::Rle,
              CompressionAlgorithm::Huffman})
        {
            for (auto encryption :
                 {EncryptionAlgorithm::None, EncryptionAlgorithm::ChaCha20,
                  EncryptionAlgorithm::Aes256})
            {
                BackupOptions options;
                options.pack = pack;
                options.compression = compression;
                options.encryption = encryption;
                options.password = "hardlink-password";
                const auto name = toString(pack) + "-" + toString(compression) +
                                  "-" + toString(encryption);
                const auto archive = temp.path / (name + ".bak");
                check(BackupEngine::create(source.string(), archive.string(),
                                           options)
                          .success,
                      "hardlink backup failed");
                check(BackupEngine::inspect(archive.string()).version == 2,
                      "hardlinks were not written with version 2");
                const auto destination = temp.path / name;
                RestoreOptions restore;
                restore.password = options.password;
                const auto result = BackupEngine::restore(
                    archive.string(), destination.string(), restore);
                check(result.success,
                      "hardlink restore failed: " + result.message);
                struct stat original
                {
                }, alias{};
                check(::stat((destination / "source/original").c_str(),
                             &original) == 0 &&
                          ::stat((destination / "source/nested/alias").c_str(),
                                 &alias) == 0,
                      "restored hardlink paths missing");
                check(original.st_dev == alias.st_dev &&
                          original.st_ino == alias.st_ino,
                      "restore lost the hardlink relationship");
                check(readBytes(destination / "source/original") ==
                          std::vector<uint8_t>({'h', 'a', 'r', 'd'}),
                      "hardlink content changed");
                const auto preview = BackupEngine::preview(
                    archive.string(), destination.string(), options.password);
                check(std::any_of(preview.entries.begin(),
                                  preview.entries.end(), [](const auto& entry)
                                  { return entry.type == "hardlink"; }),
                      "preview did not identify hardlinks");
                restore.overwrite = true;
                check(BackupEngine::restore(archive.string(),
                                            destination.string(), restore)
                          .success,
                      "hardlink overwrite failed");
            }
        }
    }

    const auto plain = temp.path / "stream-none-none.bak";
    const auto validBytes = readBytes(plain);
    auto bytes = validBytes;
    bytes[4] = 1;
    const auto legacy = temp.path / "invalid-v1.bak";
    writeBytes(legacy, bytes);
    expectRestoreFailure(legacy, temp.path / "invalid-v1", "version 2");

    const std::string target = "source/nested/alias";
    for (const bool escape : {false, true})
    {
        bytes = validBytes;
        const auto found = std::find_end(bytes.begin() + 112, bytes.end(),
                                         target.begin(), target.end());
        check(found != bytes.end(), "cannot locate hardlink reference");
        std::string replacement(target.size(), 'x');
        if (escape)
        {
            replacement.replace(0, 3, "../");
        }
        std::copy(replacement.begin(), replacement.end(), found);
        refreshPayloadDigests(bytes, true);
        const auto malformed =
            temp.path / (escape ? "escape.bak" : "missing.bak");
        writeBytes(malformed, bytes);
        const auto destination = temp.path / (escape ? "escape" : "missing");
        expectRestoreFailure(malformed, destination, "hardlink");
        check(!fs::exists(destination),
              "invalid hardlink created an output tree");
    }

    const auto raced = temp.path / "raced";
    bool replaced = false;
    RestoreOptions race;
    race.progress = [&](const ProgressEvent& event)
    {
        if (!replaced && event.stage == "restore-entry" &&
            event.detail == target)
        {
            fs::rename(raced / target, temp.path / "original-inode");
            writeBytes(raced / target, {'n', 'e', 'w'});
            replaced = true;
        }
    };
    const auto result =
        BackupEngine::restore(plain.string(), raced.string(), race);
    check(replaced && !result.success &&
              result.message.find("target changed") != std::string::npos,
          "hardlink restore accepted a replaced target");
    check(!fs::exists(raced / "source/original"),
          "raced hardlink was committed");

    bool sourceChanged = false;
    BackupOptions mutation;
    mutation.progress = [&](const ProgressEvent& event)
    {
        if (!sourceChanged && event.stage == "pack")
        {
            fs::rename(source / "original", temp.path / "old-alias");
            writeBytes(source / "original", {'h', 'a', 'r', 'd'});
            sourceChanged = true;
        }
    };
    const auto changedArchive = temp.path / "changed.bak";
    const auto changed = BackupEngine::create(
        source.string(), changedArchive.string(), mutation);
    check(sourceChanged && !changed.success && !fs::exists(changedArchive),
          "changed source hardlink produced a successful archive");
}
