#pragma once
#include "backup/core.hpp"
#include "backup/network.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <thread>
#include <stdexcept>
#include <iostream>
#include <fstream>
#include <cstring>
#include <chrono>
#include <array>
namespace fs = std::filesystem;
using namespace backup;

void check(bool condition, const std::string& message);

struct TempDirectory {
    fs::path path;
    TempDirectory();
    ~TempDirectory();
};

void writeBytes(const fs::path& path, const std::vector<uint8_t>& data);
std::vector<uint8_t> readBytes(const fs::path& path);
void makeSocketNode(const fs::path& path);
void createFixture(const fs::path& root);
std::vector<std::string> listTree(const fs::path& root);
void normalizeAtimes(const fs::path& root, const std::vector<std::string>& paths);

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
std::vector<uint8_t> readWithoutAtime(const fs::path& path, uint64_t size);
TreeSnapshot snapshotKnownTree(const fs::path& root, const std::vector<std::string>& expectedPaths);
bool sameTime(const timespec& left, const timespec& right);
void compareCompleteTree(const fs::path& restored, const std::vector<std::string>& expectedPaths,
                         const TreeSnapshot& expected);
extern bool gSocketFixtureAvailable;

struct BuiltArchive {
    fs::path path;
    PackAlgorithm pack;
    CompressionAlgorithm compression;
    EncryptionAlgorithm encryption;
};
void testSha256();
std::vector<BuiltArchive> testAllCombinations(const fs::path& workspace, const fs::path& source,
                                              const std::vector<std::string>& fixturePaths);
void testTransformBounds(const fs::path& workspace, const std::vector<BuiltArchive>& archives);
void testIndexBounds(const fs::path& workspace, const std::vector<BuiltArchive>& archives);
void testRestoreRaces(const fs::path& workspace, const fs::path& source,
                      const std::vector<BuiltArchive>& archives);
void testFailureModes(const fs::path& workspace, const fs::path& source,
                      const std::vector<BuiltArchive>& archives);
void testNetwork(const fs::path& workspace, const fs::path& archive);
const BuiltArchive& findArchive(const std::vector<BuiltArchive>& archives, PackAlgorithm pack, CompressionAlgorithm comp, EncryptionAlgorithm enc);
uint64_t readLe64(const std::vector<uint8_t>& bytes, size_t offset);
void writeLe64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value);
void refreshPayloadDigests(std::vector<uint8_t>& archive, bool packedIsPayload);
void expectRestoreFailure(const fs::path& archive, const fs::path& destination, const std::string& context);
uint16_t reservePort();
