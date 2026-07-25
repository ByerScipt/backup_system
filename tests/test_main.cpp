#include "backup/core.hpp"
#include "backup/network.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace backup;

namespace {

bool gSocketFixtureAvailable = false;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct TempDirectory {
    fs::path path;

    TempDirectory() {
        std::string pattern = (fs::temp_directory_path() / "backup-tests-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        char* created = ::mkdtemp(buffer.data());
        check(created != nullptr, "mkdtemp failed");
        path = created;
    }

    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void writeBytes(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    check(out.good(), "cannot write fixture: " + path.string());
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    check(out.good(), "cannot finish fixture: " + path.string());
}

std::vector<uint8_t> readBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    check(in.good(), "cannot read file: " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void makeSocketNode(const fs::path& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    check(fd >= 0, "socket fixture failed");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    check(path.string().size() < sizeof(address.sun_path), "socket fixture path too long");
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    int result = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    int error = errno;
    ::close(fd);
    if (result == 0) {
        gSocketFixtureAvailable = true;
        return;
    }
    if (error == EPERM || error == EACCES) {
        std::cout << "SKIP: Unix socket nodes are blocked by this sandbox\n";
        return;
    }
    check(false, "socket fixture bind failed: " + std::string(std::strerror(error)));
}

void createFixture(const fs::path& root) {
    fs::create_directories(root / "nested" / "emptydir");
    fs::create_directories(root / "中文目录");
    writeBytes(root / "empty.bin", {});
    writeBytes(root / "hello.txt", {'h', 'e', 'l', 'l', 'o', '\n'});
    writeBytes(root / "executable.sh", {'#', '!', '/', 'b', 'i', 'n', '/', 's', 'h', '\n'});
    check(::chmod((root / "executable.sh").c_str(), 0751) == 0, "chmod fixture failed");

    std::vector<uint8_t> allBytes(256);
    for (int value = 0; value < 256; ++value) allBytes[value] = static_cast<uint8_t>(value);
    writeBytes(root / "all-bytes.bin", allBytes);

    std::vector<uint8_t> randomData(8192);
    uint32_t state = 0x13579bdfu;
    for (auto& byte : randomData) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<uint8_t>(state);
    }
    writeBytes(root / "random.bin", randomData);

    std::vector<uint8_t> rleBoundary;
    rleBoundary.insert(rleBoundary.end(), 127, 'A');
    rleBoundary.insert(rleBoundary.end(), 128, 'B');
    rleBoundary.insert(rleBoundary.end(), 2, 'C');
    rleBoundary.insert(rleBoundary.end(), 3, 'D');
    rleBoundary.insert(rleBoundary.end(), 129, 'E');
    writeBytes(root / "rle-boundaries.bin", rleBoundary);
    writeBytes(root / "nested" / "single-symbol.bin", std::vector<uint8_t>(4096, 0x41));
    writeBytes(root / "中文目录" / "数据.txt", {'U', 'T', 'F', '-', '8', '\n'});

    fs::path deep = root;
    for (int depth = 0; depth < 8; ++depth) {
        deep /= "depth-" + std::to_string(depth);
        fs::create_directory(deep);
    }
    writeBytes(deep / (std::string(180, 'x') + ".bin"), {'l', 'o', 'n', 'g', '\n'});

    check(::chmod((root / "hello.txt").c_str(), 0600) == 0, "file mode fixture failed");
    timespec executableTimes[2]{{1700000000, 123456789}, {1700000001, 987654321}};
    check(::utimensat(AT_FDCWD, (root / "executable.sh").c_str(), executableTimes, 0) == 0,
          "timestamp fixture failed");
    check(::symlink("../hello.txt", (root / "nested" / "hello-link").c_str()) == 0,
          "symlink fixture failed");
    check(::mkfifo((root / "named-pipe").c_str(), 0640) == 0, "FIFO fixture failed");
    makeSocketNode(root / "unix-socket");
}

std::vector<std::string> listTree(const fs::path& root) {
    std::vector<std::string> paths{"."};
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::none, error), end;
         it != end; it.increment(error)) {
        check(!error, "cannot enumerate fixture tree");
        paths.push_back(it->path().lexically_relative(root).generic_string());
    }
    check(!error, "cannot finish fixture enumeration");
    std::sort(paths.begin(), paths.end());
    return paths;
}

void normalizeAtimes(const fs::path& root, const std::vector<std::string>& paths) {
    for (size_t index = 0; index < paths.size(); ++index) {
        fs::path path = paths[index] == "." ? root : root / paths[index];
        struct stat state{};
        check(::lstat(path.c_str(), &state) == 0, "cannot stat fixture for atime normalization");
        timespec times[2]{
            {1700001000 + static_cast<time_t>(index), static_cast<long>((index * 7919) % 1000000000)},
            state.st_mtim
        };
        int flags = S_ISLNK(state.st_mode) ? AT_SYMLINK_NOFOLLOW : 0;
        check(::utimensat(AT_FDCWD, path.c_str(), times, flags) == 0,
              "cannot normalize fixture atime");
    }
}

struct TreeEntry {
    mode_t type = 0;
    mode_t mode = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    uint64_t size = 0;
    timespec atime{};
    timespec mtime{};
    std::string linkTarget;
    std::vector<uint8_t> content;
};

using TreeSnapshot = std::map<std::string, TreeEntry>;

std::vector<uint8_t> readWithoutAtime(const fs::path& path, uint64_t size) {
    int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
#ifdef O_NOATIME
    flags |= O_NOATIME;
#endif
    int fd = ::open(path.c_str(), flags);
    check(fd >= 0, "cannot securely read tree entry: " + path.string());
    std::vector<uint8_t> data(static_cast<size_t>(size));
    size_t done = 0;
    while (done < data.size()) {
        ssize_t count = ::read(fd, data.data() + done, data.size() - done);
        if (count < 0 && errno == EINTR) continue;
        check(count > 0, "truncated tree entry: " + path.string());
        done += static_cast<size_t>(count);
    }
    check(::close(fd) == 0, "cannot close tree entry");
    return data;
}

TreeSnapshot snapshotKnownTree(const fs::path& root,
                               const std::vector<std::string>& expectedPaths) {
    TreeSnapshot snapshot;
    for (const std::string& relative : expectedPaths) {
        fs::path path = relative == "." ? root : root / relative;
        struct stat state{};
        check(::lstat(path.c_str(), &state) == 0, "tree entry is missing: " + relative);
        TreeEntry entry;
        entry.type = state.st_mode & S_IFMT;
        entry.mode = state.st_mode & 07777;
        entry.uid = state.st_uid;
        entry.gid = state.st_gid;
        entry.size = S_ISREG(state.st_mode) ? static_cast<uint64_t>(state.st_size) : 0;
        entry.atime = state.st_atim;
        entry.mtime = state.st_mtim;
        if (S_ISREG(state.st_mode)) entry.content = readWithoutAtime(path, entry.size);
        if (S_ISLNK(state.st_mode)) {
            std::vector<char> target(4096);
            ssize_t length = ::readlink(path.c_str(), target.data(), target.size());
            check(length >= 0 && static_cast<size_t>(length) < target.size(),
                  "cannot read tree symlink");
            entry.linkTarget.assign(target.data(), static_cast<size_t>(length));
            timespec preserve[2]{state.st_atim, state.st_mtim};
            check(::utimensat(AT_FDCWD, path.c_str(), preserve, AT_SYMLINK_NOFOLLOW) == 0,
                  "cannot preserve symlink timestamps while comparing");
        }
        snapshot.emplace(relative, std::move(entry));
    }
    return snapshot;
}

bool sameTime(const timespec& left, const timespec& right) {
    return left.tv_sec == right.tv_sec && left.tv_nsec == right.tv_nsec;
}

void compareCompleteTree(const fs::path& restored,
                         const std::vector<std::string>& expectedPaths,
                         const TreeSnapshot& expected) {
    TreeSnapshot actual = snapshotKnownTree(restored, expectedPaths);
    check(listTree(restored) == expectedPaths,
          "restored tree has missing or unexpected entries");
    check(actual.size() == expected.size(), "restored tree entry count mismatch");
    for (const auto& [path, source] : expected) {
        const TreeEntry& target = actual.at(path);
        check(target.type == source.type, "entry type mismatch: " + path);
        check(target.mode == source.mode, "entry mode mismatch: " + path);
        check(target.uid == source.uid && target.gid == source.gid,
              "entry ownership mismatch: " + path);
        check(target.size == source.size, "entry size mismatch: " + path);
        check(target.content == source.content, "entry content mismatch: " + path);
        check(target.linkTarget == source.linkTarget, "link target mismatch: " + path);
        check(sameTime(target.atime, source.atime), "nanosecond atime mismatch: " + path);
        check(sameTime(target.mtime, source.mtime), "nanosecond mtime mismatch: " + path);
    }
}

void testSha256() {
    check(hexDigest(sha256(std::string("abc"))) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 known vector failed");
}

struct BuiltArchive {
    fs::path path;
    PackAlgorithm pack;
    CompressionAlgorithm compression;
    EncryptionAlgorithm encryption;
};

std::vector<BuiltArchive> testAllCombinations(
    const fs::path& workspace, const fs::path& source,
    const std::vector<std::string>& fixturePaths) {
    std::vector<BuiltArchive> archives;
    int number = 0;
    for (auto pack : {PackAlgorithm::Stream, PackAlgorithm::Index}) {
        for (auto compression : {CompressionAlgorithm::None, CompressionAlgorithm::Rle,
                                 CompressionAlgorithm::Huffman}) {
            for (auto encryption : {EncryptionAlgorithm::None, EncryptionAlgorithm::Xor,
                                    EncryptionAlgorithm::Vigenere}) {
                normalizeAtimes(source, fixturePaths);
                TreeSnapshot expected = snapshotKnownTree(source, fixturePaths);
                fs::path archive = workspace / ("combo-" + std::to_string(number) + ".bak");
                BackupOptions options;
                options.pack = pack;
                options.compression = compression;
                options.encryption = encryption;
                if (encryption != EncryptionAlgorithm::None) {
                    options.password = "correct horse battery staple";
                }
                auto created = BackupEngine::create(source.string(), archive.string(), options);
                check(created.success, "combination backup failed: " + created.message);
                auto info = BackupEngine::inspect(archive.string());
                check(info.pack == pack && info.compression == compression &&
                      info.encryption == encryption, "inspect algorithm mismatch");

                fs::path destination = workspace / ("restore-" + std::to_string(number));
                RestoreOptions restore;
                restore.password = options.password;
                auto restored = BackupEngine::restore(archive.string(), destination.string(), restore);
                check(restored.success, "combination restore failed: " + restored.message);
                compareCompleteTree(destination / source.filename(), fixturePaths, expected);
                archives.push_back({archive, pack, compression, encryption});
                ++number;
            }
        }
    }
    check(archives.size() == 18, "not all 18 algorithm combinations ran");
    return archives;
}

const BuiltArchive& findArchive(const std::vector<BuiltArchive>& archives,
                                PackAlgorithm pack, CompressionAlgorithm compression,
                                EncryptionAlgorithm encryption) {
    auto found = std::find_if(archives.begin(), archives.end(), [&](const auto& archive) {
        return archive.pack == pack && archive.compression == compression &&
               archive.encryption == encryption;
    });
    check(found != archives.end(), "required archive fixture is missing");
    return *found;
}

uint64_t readLe64(const std::vector<uint8_t>& bytes, size_t offset) {
    check(offset + 8 <= bytes.size(), "test field is out of range");
    uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8);
    }
    return value;
}

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

uint16_t reservePort() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 && (errno == EPERM || errno == EACCES)) {
        std::cout << "SKIP: TCP sockets are blocked by this sandbox\n";
        return 0;
    }
    check(fd >= 0, "port reservation socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    int bound = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    int error = errno;
    if (bound != 0 && (error == EPERM || error == EACCES)) {
        ::close(fd);
        std::cout << "SKIP: TCP bind is blocked by this sandbox\n";
        return 0;
    }
    check(bound == 0, "port reservation bind failed");
    socklen_t size = sizeof(address);
    check(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0,
          "getsockname failed");
    uint16_t port = ntohs(address.sin_port);
    ::close(fd);
    return port;
}

void testNetwork(const fs::path& workspace, const fs::path& archive) {
    uint16_t port = reservePort();
    if (port == 0) return;
    network::ServerConfig config;
    config.port = port;
    config.storagePath = (workspace / "server-data").string();
    config.timeoutSeconds = 3;
    network::BackupServer server(config);
    std::thread serverThread([&] { check(server.run(), "server run failed"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    try {
        std::string error;
        network::BackupClient alice("127.0.0.1", port, "alice", "alice-password");
        check(alice.registerUser(error), "Alice registration failed: " + error);
        error.clear();
        check(!alice.registerUser(error), "duplicate registration was accepted");
        error.clear();

        std::string id;
        check(alice.upload(archive.string(), "integration backup", id, error),
              "network upload failed: " + error);
        check(id.size() == 32, "server returned invalid backup ID");
        error.clear();
        auto entries = alice.list(error);
        check(error.empty() && entries.size() == 1 && entries[0].id == id,
              "network list mismatch: " + error);
        fs::path downloaded = workspace / "downloaded.bak";
        check(alice.download(id, downloaded.string(), error),
              "network download failed: " + error);
        check(sha256File(downloaded.string()) == sha256File(archive.string()),
              "network roundtrip checksum mismatch");

        fs::path brokenDownload = workspace / "broken-download.bak";
        check(::symlink("missing-download", brokenDownload.c_str()) == 0,
              "broken download symlink fixture failed");
        error.clear();
        check(!alice.download(id, brokenDownload.string(), error),
              "network download overwrote a broken symlink");
        check(fs::is_symlink(brokenDownload),
              "network download did not preserve broken symlink");

        struct stat state{};
        check(::stat((workspace / "server-data" / "users.db").c_str(), &state) == 0 &&
              (state.st_mode & 0777) == 0600, "user database permissions are not 0600");
        fs::path aliceDirectory = workspace / "server-data" / "users" /
            hexDigest(sha256(std::string("alice")));
        check(::stat(aliceDirectory.c_str(), &state) == 0 &&
              (state.st_mode & 0777) == 0700, "user storage directory is not 0700");
        check(::stat((aliceDirectory / (id + ".bak")).c_str(), &state) == 0 &&
              (state.st_mode & 0777) == 0600, "stored archive is not 0600");
        check(::stat((aliceDirectory / (id + ".meta")).c_str(), &state) == 0 &&
              (state.st_mode & 0777) == 0600, "backup metadata is not 0600");

        network::BackupClient wrong("127.0.0.1", port, "alice", "wrong");
        error.clear();
        auto denied = wrong.list(error);
        check(denied.empty() && !error.empty(), "wrong account password was accepted");
        network::BackupClient bob("127.0.0.1", port, "bob", "bob-password");
        error.clear();
        check(bob.registerUser(error), "Bob registration failed: " + error);
        error.clear();
        check(bob.list(error).empty() && error.empty(), "cross-user list isolation failed");
        error.clear();
        check(!bob.download(id, (workspace / "bob-download.bak").string(), error),
              "cross-user download was accepted");

        fs::path cancelledUpload = workspace / "cancelled-upload.bin";
        writeBytes(cancelledUpload, std::vector<uint8_t>(2 * 1024 * 1024, 0x5a));
        std::atomic_bool cancel{false};
        std::string cancelledId;
        auto cancelAfterChunk = [&](const ProgressEvent& event) {
            if (event.completed >= 512 * 1024) cancel.store(true);
        };
        error.clear();
        check(!alice.upload(cancelledUpload.string(), "cancelled", cancelledId, error,
                            cancelAfterChunk, &cancel),
              "cancelled upload unexpectedly completed");
        check(error.find("cancelled") != std::string::npos,
              "cancelled upload returned the wrong error");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        error.clear();
        entries = alice.list(error);
        check(error.empty() && entries.size() == 1,
              "cancelled upload left a visible half-finished backup");

        for (int attempt = 0; attempt < 100; ++attempt) {
            error.clear();
            entries = alice.list(error);
            check(error.empty() && entries.size() == 1,
                  "server failed while recycling completed sessions");
        }
    } catch (...) {
        server.stop();
        serverThread.join();
        throw;
    }
    server.stop();
    serverThread.join();

    network::BackupServer restarted(config);
    std::thread restartThread([&] { check(restarted.run(), "restarted server run failed"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    try {
        std::string error;
        network::BackupClient alice("127.0.0.1", port, "alice", "alice-password");
        auto entries = alice.list(error);
        check(error.empty() && entries.size() == 1,
              "server restart did not preserve backup history: " + error);
    } catch (...) {
        restarted.stop();
        restartThread.join();
        throw;
    }
    restarted.stop();
    restartThread.join();
}

} // namespace

int main() {
    try {
        std::cout << "[1/4] SHA-256\n";
        testSha256();
        TempDirectory temp;
        fs::path source = temp.path / "testsrc";
        createFixture(source);
        std::vector<std::string> fixturePaths = listTree(source);

        std::cout << "[2/4] 18 complete-tree algorithm combinations\n";
        auto archives = testAllCombinations(temp.path, source, fixturePaths);
        std::cout << "[3/4] corruption, bounds, conflicts, and path races\n";
        testFailureModes(temp.path, source, archives);
        std::cout << "[4/4] account-isolated network roundtrip and session recycling\n";
        testNetwork(temp.path, archives.front().path);
        std::cout << "All backup-system tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
