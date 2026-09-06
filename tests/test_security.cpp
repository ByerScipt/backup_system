#include "helpers.hpp"



namespace fs = std::filesystem;
using namespace backup;

void writeLe64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    check(offset + 8 <= bytes.size(), "test field is out of range");
    for (int index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

void refreshPayloadDigests(std::vector<uint8_t>& archive, bool packedIsPayload) {
    check(archive.size() >= 112, "test archive is too small");
    std::vector<uint8_t> payload(archive.begin() + 112, archive.end());
    auto digest = sha256(payload);
    std::copy(digest.begin(), digest.end(), archive.begin() + 80);
    if (packedIsPayload) std::copy(digest.begin(), digest.end(), archive.begin() + 48);
}

void expectRestoreFailure(const fs::path& archive, const fs::path& destination,
                          const std::string& expectedMessage = {}) {
    auto result = BackupEngine::restore(archive.string(), destination.string(), {});
    check(!result.success, "malformed archive was restored: " + archive.filename().string());
    if (!expectedMessage.empty()) {
        check(result.message.find(expectedMessage) != std::string::npos,
              "unexpected rejection reason: " + result.message);
    }
}

void testTransformBounds(const fs::path& workspace,
                         const std::vector<BuiltArchive>& archives) {
    for (auto compression : {CompressionAlgorithm::Rle, CompressionAlgorithm::Huffman}) {
        const auto& source = findArchive(archives, PackAlgorithm::Stream, compression,
                                         EncryptionAlgorithm::None);
        std::vector<uint8_t> bytes = readBytes(source.path);
        uint64_t packedSize = readLe64(bytes, 16);
        writeLe64(bytes, 116, packedSize + 1);
        refreshPayloadDigests(bytes, false);
        fs::path malformed = workspace /
            (compression == CompressionAlgorithm::Rle ? "rle-over-output.bak"
                                                      : "huffman-over-output.bak");
        writeBytes(malformed, bytes);
        expectRestoreFailure(
            malformed, workspace / (malformed.stem().string() + "-target"),
            compression == CompressionAlgorithm::Rle ? "RLE declared output size"
                                                     : "Huffman declared output size");
    }

    const auto& huffman = findArchive(archives, PackAlgorithm::Stream,
                                      CompressionAlgorithm::Huffman,
                                      EncryptionAlgorithm::None);
    std::vector<uint8_t> emptyArchive = readBytes(huffman.path);
    emptyArchive.resize(112);
    std::vector<uint8_t> emptyPayload{'H', 'U', 'F', '1'};
    emptyPayload.resize(4 + 8 + 256, 0);
    emptyArchive.insert(emptyArchive.end(), emptyPayload.begin(), emptyPayload.end());
    writeLe64(emptyArchive, 16, 0);
    writeLe64(emptyArchive, 24, emptyPayload.size());
    auto emptyDigest = sha256(std::vector<uint8_t>{});
    std::copy(emptyDigest.begin(), emptyDigest.end(), emptyArchive.begin() + 48);
    refreshPayloadDigests(emptyArchive, false);
    fs::path empty = workspace / "huffman-empty.bak";
    writeBytes(empty, emptyArchive);
    auto emptyResult = BackupEngine::restore(
        empty.string(), (workspace / "huffman-empty-target").string(), {});
    check(!emptyResult.success &&
          emptyResult.message.find("Huffman") == std::string::npos,
          "empty Huffman stream did not pass decompression safely");

    std::vector<uint8_t> padding = readBytes(huffman.path);
    padding.back() ^= 1;
    refreshPayloadDigests(padding, false);
    fs::path nonZeroPadding = workspace / "huffman-nonzero-padding.bak";
    writeBytes(nonZeroPadding, padding);
    expectRestoreFailure(nonZeroPadding, workspace / "huffman-padding-target");
}

void testIndexBounds(const fs::path& workspace,
                     const std::vector<BuiltArchive>& archives) {
    const auto& source = findArchive(archives, PackAlgorithm::Index,
                                     CompressionAlgorithm::None,
                                     EncryptionAlgorithm::None);
    std::vector<uint8_t> bytes = readBytes(source.path);
    size_t trailer = bytes.size() - 32;
    uint64_t centralSize = readLe64(bytes, trailer + 16);
    check(centralSize > 0, "central directory fixture is empty");
    writeLe64(bytes, trailer + 16, centralSize - 1);
    refreshPayloadDigests(bytes, true);
    fs::path badSize = workspace / "index-central-size.bak";
    writeBytes(badSize, bytes);
    expectRestoreFailure(badSize, workspace / "index-central-size-target",
                         "central directory size");

    bytes = readBytes(source.path);
    bytes.back() = 1;
    refreshPayloadDigests(bytes, true);
    fs::path reserved = workspace / "index-reserved.bak";
    writeBytes(reserved, bytes);
    expectRestoreFailure(reserved, workspace / "index-reserved-target",
                         "reserved field");
}

void testRestoreRaces(const fs::path& workspace, const fs::path& source,
                      const std::vector<BuiltArchive>& archives) {
    const auto& plain = findArchive(archives, PackAlgorithm::Stream,
                                    CompressionAlgorithm::None,
                                    EncryptionAlgorithm::None);
    fs::path conflictDestination = workspace / "late-conflict";
    fs::path lateTarget = conflictDestination / source.filename() / "hello.txt";
    bool conflictInjected = false;
    RestoreOptions noOverwrite;
    noOverwrite.progress = [&](const ProgressEvent& event) {
        if (!conflictInjected && event.stage == "extract") {
            writeBytes(lateTarget, {'k', 'e', 'e', 'p'});
            conflictInjected = true;
        }
    };
    auto conflictResult = BackupEngine::restore(
        plain.path.string(), conflictDestination.string(), noOverwrite);
    check(conflictInjected, "late conflict fixture was not injected");
    check(!conflictResult.success, "late target conflict was overwritten by default");
    check(readBytes(lateTarget) == std::vector<uint8_t>({'k', 'e', 'e', 'p'}),
          "late target conflict content was changed");

    fs::path symlinkDestination = workspace / "parent-race";
    fs::path outside = workspace / "outside";
    fs::create_directory(outside);
    bool symlinkInjected = false;
    RestoreOptions race;
    race.progress = [&](const ProgressEvent& event) {
        if (!symlinkInjected && event.stage == "extract") {
            fs::path parent = symlinkDestination / source.filename() / "中文目录";
            check(::rmdir(parent.c_str()) == 0, "cannot remove parent race fixture");
            check(::symlink(outside.c_str(), parent.c_str()) == 0,
                  "cannot install parent race symlink");
            symlinkInjected = true;
        }
    };
    auto raceResult = BackupEngine::restore(
        plain.path.string(), symlinkDestination.string(), race);
    check(symlinkInjected, "parent symlink race fixture was not injected");
    check(!raceResult.success, "symlink parent race escaped restore destination");
    check(!fs::exists(outside / "数据.txt"), "restore wrote through raced symlink parent");
}

void testFailureModes(const fs::path& workspace, const fs::path& source,
                      const std::vector<BuiltArchive>& archives) {
    const auto& encrypted = findArchive(archives, PackAlgorithm::Stream,
                                        CompressionAlgorithm::Huffman,
                                        EncryptionAlgorithm::Xor);
    RestoreOptions wrong;
    wrong.password = "wrong password";
    auto wrongResult = BackupEngine::restore(
        encrypted.path.string(), (workspace / "wrong-password").string(), wrong);
    check(!wrongResult.success, "wrong password was accepted");
    check(!fs::exists(workspace / "wrong-password" / source.filename()),
          "wrong password created output tree");

    const auto& plain = findArchive(archives, PackAlgorithm::Stream,
                                    CompressionAlgorithm::None,
                                    EncryptionAlgorithm::None);
    fs::path conflict = workspace / "conflict";
    auto first = BackupEngine::restore(plain.path.string(), conflict.string(), {});
    check(first.success, "initial conflict fixture restore failed");
    auto second = BackupEngine::restore(plain.path.string(), conflict.string(), {});
    check(!second.success, "restore conflict was not rejected");
    RestoreOptions overwrite;
    overwrite.overwrite = true;
    auto third = BackupEngine::restore(plain.path.string(), conflict.string(), overwrite);
    check(third.success, "explicit overwrite failed: " + third.message);
    auto preview = BackupEngine::preview(plain.path.string(), conflict.string());
    check(preview.entries.size() >= 10 && !preview.conflicts.empty(),
          "restore preview omitted entries or conflicts");

    fs::path corrupt = workspace / "corrupt.bak";
    std::vector<uint8_t> bytes = readBytes(plain.path);
    bytes.back() ^= 0x5a;
    writeBytes(corrupt, bytes);
    bool rejected = false;
    try {
        static_cast<void>(BackupEngine::inspect(corrupt.string()));
    } catch (...) {
        rejected = true;
    }
    check(rejected, "corrupt archive passed inspect");

    fs::path truncated = workspace / "truncated.bak";
    writeBytes(truncated, std::vector<uint8_t>(bytes.begin(), bytes.begin() + 50));
    rejected = false;
    try {
        static_cast<void>(BackupEngine::inspect(truncated.string()));
    } catch (...) {
        rejected = true;
    }
    check(rejected, "truncated archive passed inspect");

    bytes = readBytes(plain.path);
    bytes[12] = 9;
    fs::path invalidAlgorithm = workspace / "invalid-algorithm.bak";
    writeBytes(invalidAlgorithm, bytes);
    rejected = false;
    try {
        static_cast<void>(BackupEngine::inspect(invalidAlgorithm.string()));
    } catch (...) {
        rejected = true;
    }
    check(rejected, "invalid algorithm identifier passed inspect");

    BackupOptions basicOptions;
    auto insideResult = BackupEngine::create(
        source.string(), (source / "inside.bak").string(), basicOptions);
    check(!insideResult.success, "archive inside source was accepted");
    fs::path brokenOutput = workspace / "broken-output.bak";
    check(::symlink("missing-target", brokenOutput.c_str()) == 0,
          "broken output symlink fixture failed");
    auto brokenResult = BackupEngine::create(
        source.string(), brokenOutput.string(), basicOptions);
    check(!brokenResult.success && fs::is_symlink(brokenOutput),
          "broken output symlink was overwritten");

    fs::path mutationSource = workspace / "mutation-src";
    fs::create_directory(mutationSource);
    fs::path mutationFile = mutationSource / "mutable.bin";
    writeBytes(mutationFile, {'o', 'l', 'd', '!'});
    struct stat originalState{};
    check(::stat(mutationFile.c_str(), &originalState) == 0,
          "cannot stat mutation fixture");
    bool replaced = false;
    BackupOptions mutationOptions;
    mutationOptions.progress = [&](const ProgressEvent& event) {
        if (!replaced && event.stage == "scan" &&
            event.detail == "mutation-src/mutable.bin") {
            fs::rename(mutationFile, workspace / "mutation-original.bin");
            writeBytes(mutationFile, {'n', 'e', 'w', '!'});
            timespec times[2]{originalState.st_atim, originalState.st_mtim};
            check(::utimensat(AT_FDCWD, mutationFile.c_str(), times, 0) == 0,
                  "cannot preserve mutation fixture times");
            replaced = true;
        }
    };
    auto mutationResult = BackupEngine::create(
        mutationSource.string(), (workspace / "mutation.bak").string(), mutationOptions);
    check(replaced && !mutationResult.success,
          "same-size same-mtime source replacement was accepted");

    std::string root = source.filename().string();
    check(root.size() == 7, "fixture root name must be seven bytes");
    std::vector<uint8_t> malicious = readBytes(plain.path);
    auto rootAt = std::search(malicious.begin() + 112, malicious.end(),
                              root.begin(), root.end());
    check(rootAt != malicious.end(), "cannot locate root path fixture");
    std::copy_n("../evil", 7, rootAt);
    refreshPayloadDigests(malicious, true);
    fs::path traversal = workspace / "traversal.bak";
    writeBytes(traversal, malicious);
    expectRestoreFailure(traversal, workspace / "path-target");
    check(!fs::exists(workspace / "evil"), "path traversal escaped destination");

    std::vector<uint8_t> duplicate = readBytes(plain.path);
    std::string original = root + "/hello.txt";
    std::string replacement = root + "/empty.bin";
    check(original.size() == replacement.size(), "duplicate fixture lengths differ");
    auto duplicateAt = std::search(duplicate.begin() + 112, duplicate.end(),
                                   original.begin(), original.end());
    check(duplicateAt != duplicate.end(), "cannot locate duplicate path fixture");
    std::copy(replacement.begin(), replacement.end(), duplicateAt);
    refreshPayloadDigests(duplicate, true);
    fs::path duplicateArchive = workspace / "duplicate.bak";
    writeBytes(duplicateArchive, duplicate);
    expectRestoreFailure(duplicateArchive, workspace / "duplicate-target");

    std::vector<uint8_t> oversized = readBytes(plain.path);
    rootAt = std::search(oversized.begin() + 112, oversized.end(),
                         root.begin(), root.end());
    check(rootAt != oversized.end() && rootAt - oversized.begin() >= 4,
          "cannot locate path length fixture");
    std::fill(rootAt - 4, rootAt, 0xff);
    refreshPayloadDigests(oversized, true);
    fs::path oversizedArchive = workspace / "oversized-length.bak";
    writeBytes(oversizedArchive, oversized);
    expectRestoreFailure(oversizedArchive, workspace / "oversized-target");

    std::vector<uint8_t> symlinkEscape = readBytes(plain.path);
    std::string regularPath = root + "/nested/single-symbol.bin";
    std::string childPath = root + "/nested/hello-link/escape";
    check(regularPath.size() == childPath.size(), "symlink path fixture lengths differ");
    auto regularAt = std::search(symlinkEscape.begin() + 112, symlinkEscape.end(),
                                 regularPath.begin(), regularPath.end());
    check(regularAt != symlinkEscape.end(), "cannot locate symlink child fixture");
    std::copy(childPath.begin(), childPath.end(), regularAt);
    std::string safeTarget = "../hello.txt";
    std::string escapeTarget = "../../../out";
    auto targetAt = std::search(symlinkEscape.begin() + 112, symlinkEscape.end(),
                                safeTarget.begin(), safeTarget.end());
    check(targetAt != symlinkEscape.end(), "cannot locate symlink target fixture");
    std::copy(escapeTarget.begin(), escapeTarget.end(), targetAt);
    refreshPayloadDigests(symlinkEscape, true);
    fs::path symlinkArchive = workspace / "symlink-descendant.bak";
    writeBytes(symlinkArchive, symlinkEscape);
    expectRestoreFailure(symlinkArchive, workspace / "symlink-target");
    check(!fs::exists(workspace / "out"), "archive symlink escaped destination");

    const auto& huffman = findArchive(archives, PackAlgorithm::Stream,
                                      CompressionAlgorithm::Huffman,
                                      EncryptionAlgorithm::None);
    std::vector<uint8_t> trailing = readBytes(huffman.path);
    uint64_t encodedSize = readLe64(trailing, 24);
    trailing.push_back(0);
    writeLe64(trailing, 24, encodedSize + 1);
    refreshPayloadDigests(trailing, false);
    fs::path trailingArchive = workspace / "huffman-trailing.bak";
    writeBytes(trailingArchive, trailing);
    expectRestoreFailure(trailingArchive, workspace / "huffman-trailing-target");

    testTransformBounds(workspace, archives);
    testIndexBounds(workspace, archives);
    testRestoreRaces(workspace, source, archives);
}
