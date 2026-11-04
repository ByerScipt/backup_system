#include "helpers.hpp"
#include <cstdlib>

void testRestorePreflight()
{
    TempDirectory temp;
    const auto source = temp.path / "source";
    fs::create_directory(source);
    writeBytes(source / "a", {'n', 'e', 'w'});
    writeBytes(source / "b", {'d', 'a', 't', 'a'});
    const auto archive = temp.path / "archive.bak";
    check(BackupEngine::create(source.string(), archive.string(), {}).success,
          "cannot create preflight fixture");
    const auto destination = temp.path / "destination";
    fs::create_directories(destination / "source/b");
    writeBytes(destination / "source/a", {'o', 'l', 'd'});
    RestoreOptions options;
    options.overwrite = true;
    auto result =
        BackupEngine::restore(archive.string(), destination.string(), options);
    check(!result.success && readBytes(destination / "source/a") ==
                                 std::vector<uint8_t>({'o', 'l', 'd'}),
          "a predictable later conflict modified an earlier restore target");
    std::atomic_bool cancel{false};
    options.cancel = &cancel;
    options.progress = [&](const ProgressEvent& event)
    {
        if (event.stage == "restore-entry")
        {
            cancel = true;
        }
    };
    result = BackupEngine::restore(archive.string(),
                                   (temp.path / "cancelled").string(), options);
    check(!result.success && result.entryCount == 1 &&
              result.outputBytes == 3 &&
              result.message.find("already restored") != std::string::npos,
          "partial restore failure did not identify committed output");

    RestoreOptions snapshot;
    snapshot.progress = [&](const ProgressEvent& event)
    {
        if (event.stage == "restore-preflight")
        {
            writeBytes(archive, {'r', 'e', 'p', 'l', 'a', 'c', 'e', 'd'});
        }
    };
    result = BackupEngine::restore(archive.string(),
                                   (temp.path / "snapshot").string(), snapshot);
    check(result.success && readBytes(temp.path / "snapshot/source/b") ==
                                std::vector<uint8_t>({'d', 'a', 't', 'a'}),
          "source archive modification changed already validated restore data");

    // A full/unwritable system temporary volume must not block operations
    // whose output volume is usable. Stages belong beside their destination.
    const auto blocked = temp.path / "blocked-temp";
    fs::create_directory(blocked);
    ::chmod(blocked.c_str(), 0500);
    const char* environment = ::getenv("TMPDIR");
    const bool hadTemporary = environment != nullptr;
    const std::string previousTemporary = environment ? environment : "";
    ::setenv("TMPDIR", blocked.c_str(), 1);
    BackupOptions encrypted;
    encrypted.encryption = EncryptionAlgorithm::ChaCha20;
    encrypted.compression = CompressionAlgorithm::Lz77;
    encrypted.password = "storage-test";
    const auto secure = temp.path / "secure.bak";
    const auto created =
        BackupEngine::create(source.string(), secure.string(), encrypted);
    RestoreOptions restore;
    restore.password = encrypted.password;
    const auto restored = BackupEngine::restore(
        secure.string(), (temp.path / "storage-restored").string(), restore);
    if (hadTemporary)
    {
        ::setenv("TMPDIR", previousTemporary.c_str(), 1);
    }
    else
    {
        ::unsetenv("TMPDIR");
    }
    ::chmod(blocked.c_str(), 0700);
    check(created.success && restored.success,
          "usable destination depended on the system temporary volume: " +
              created.message + "; " + restored.message);
}
