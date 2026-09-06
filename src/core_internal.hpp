#pragma once
// Internal shared header for the core pipeline.
// Public API stays in include/backup/core.hpp; everything here is
// implementation detail (backup::detail) so each .cpp stays small.
#include "backup/core.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <fcntl.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace backup {
namespace detail {

inline constexpr size_t kBufferSize = 1024 * 1024;
inline constexpr uint64_t kMaxEntries = 1000000;
inline constexpr uint32_t kMaxString = 1024 * 1024;
inline constexpr uint16_t kArchiveVersion = 1;
inline constexpr uint16_t kArchiveHeaderSize = 112;
inline constexpr std::array<char, 4> kArchiveMagic{{'B', 'K', 'P', '2'}};
inline constexpr std::array<char, 8> kStreamMagic{{'S', 'T', 'R', 'M', 'P', 'K', '1', '\0'}};
inline constexpr std::array<char, 8> kIndexMagic{{'I', 'N', 'D', 'X', 'P', 'K', '1', '\0'}};
inline constexpr std::array<char, 8> kCentralMagic{{'C', 'D', 'I', 'R', 'V', '1', '\0', '\0'}};
inline constexpr std::array<char, 8> kIndexEndMagic{{'I', 'D', 'X', 'E', 'N', 'D', '1', '\0'}};

class BackupError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void ensure(bool condition, const std::string& message);
void checkCancelled(std::atomic_bool* cancel);
void report(const ProgressCallback& callback, const std::string& stage,
            uint64_t completed, uint64_t total, const std::string& detail = {});

// ---- io ----
void writeExact(std::ostream& out, const void* data, size_t size);
void readExact(std::istream& in, void* data, size_t size);
void writeU8(std::ostream& out, uint8_t value);
void writeU16(std::ostream& out, uint16_t value);
void writeU32(std::ostream& out, uint32_t value);
void writeU64(std::ostream& out, uint64_t value);
uint8_t readU8(std::istream& in);
uint16_t readU16(std::istream& in);
uint32_t readU32(std::istream& in);
uint64_t readU64(std::istream& in);
void writeString(std::ostream& out, const std::string& value);
std::string readString(std::istream& in);
void copyBytes(std::istream& in, std::ostream& out, uint64_t count,
               const ProgressCallback& callback = {}, const std::string& stage = {},
               std::atomic_bool* cancel = nullptr);
uint64_t fileSizeChecked(const fs::path& path);
void copyFileStage(const fs::path& input, const fs::path& output,
                   const std::string& stage, const ProgressCallback& progress,
                   std::atomic_bool* cancel);
void commitFileNoReplace(const fs::path& temporary, const fs::path& output);
bool isPathInside(const fs::path& child, const fs::path& parent);
size_t pathDepth(const fs::path& path);
void warnMetadata(const std::string& operation, const fs::path& path);

class TempFile {
public:
    explicit TempFile(const fs::path& directory = fs::temp_directory_path());
    ~TempFile();
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    const fs::path& path() const { return path_; }
    void keep() { keep_ = true; }
private:
    fs::path path_;
    bool keep_ = false;
};

// ---- sha ----
std::array<uint8_t, 32> shaFileRange(const fs::path& path, uint64_t offset, uint64_t length);

// ---- entry / scan ----
enum class EntryType : uint8_t {
    Regular = 1, Directory = 2, Symlink = 3, Fifo = 4,
    Character = 5, Block = 6, Socket = 7
};

struct Entry {
    std::string path;
    fs::path sourcePath;
    EntryType type = EntryType::Regular;
    uint32_t mode = 0;
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint64_t size = 0;
    int64_t atimeSec = 0;
    uint32_t atimeNsec = 0;
    int64_t mtimeSec = 0;
    uint32_t mtimeNsec = 0;
    int64_t ctimeSec = 0;
    uint32_t ctimeNsec = 0;
    std::string linkTarget;
    uint64_t device = 0;
    uint64_t contentOffset = 0;
    uint64_t sourceDevice = 0;
    uint64_t sourceInode = 0;
};

EntryType entryTypeFromMode(mode_t mode);
Entry makeEntry(const fs::path& full, const std::string& archivePath);
std::vector<Entry> scanDirectory(const fs::path& source, uint64_t& inputBytes,
                                 const BackupOptions& options);
void writeEntryMetadata(std::ostream& out, const Entry& e, bool withOffset);
Entry readEntryMetadata(std::istream& in, bool withOffset);
bool sameFileState(const struct stat& st, const Entry& e);
void copySourceFile(const Entry& e, std::ostream& out, uint64_t& completed,
                    uint64_t total, const BackupOptions& options);
bool safeArchivePath(const std::string& value);
void validateEntries(std::vector<Entry>& entries, uint64_t packedSize);

// ---- pack ----
void packStream(const std::vector<Entry>& entries, const fs::path& output,
                uint64_t inputBytes, const BackupOptions& options);
void packIndex(std::vector<Entry> entries, const fs::path& output,
               uint64_t inputBytes, const BackupOptions& options);
std::vector<Entry> readStreamEntries(std::ifstream& in, uint64_t size);
std::vector<Entry> readIndexEntries(std::ifstream& in, uint64_t size);
std::vector<Entry> readPackedEntries(const fs::path& packed, PackAlgorithm algorithm);

// ---- compress ----
void rleCompress(const fs::path& input, const fs::path& output,
                 const BackupOptions& options);
void rleDecompress(const fs::path& input, const fs::path& output,
                   uint64_t expectedSize, const RestoreOptions& options);
void huffmanCompress(const fs::path& input, const fs::path& output,
                     const BackupOptions& options);
void huffmanDecompress(const fs::path& input, const fs::path& output,
                       uint64_t expectedSize, const RestoreOptions& options);
void compressStage(const fs::path& input, const fs::path& output,
                   CompressionAlgorithm algorithm, const BackupOptions& options);
void decompressStage(const fs::path& input, const fs::path& output,
                     CompressionAlgorithm algorithm, uint64_t expectedSize,
                     const RestoreOptions& options);

// ---- crypto ----
std::array<uint8_t, 32> deriveKey(const std::string& password,
                                  const std::array<uint8_t,16>& salt);
void cryptStage(const fs::path& input, const fs::path& output,
                EncryptionAlgorithm algorithm, const std::string& password,
                const std::array<uint8_t,16>& salt,
                const ProgressCallback& progress, std::atomic_bool* cancel,
                const std::string& stage);
std::array<uint8_t,16> randomSalt();

// ---- archive ----
struct ParsedHeader {
    ArchiveInfo info;
    std::array<uint8_t,16> salt{};
};
void writeArchiveHeader(std::ostream& out, const ArchiveInfo& info,
                        const std::array<uint8_t,16>& salt);
ParsedHeader readArchiveHeader(std::istream& in);
ParsedHeader readArchiveHeader(const fs::path& path);
using DecodedArchiveConsumer =
    std::function<void(const ParsedHeader&, const fs::path&, const std::vector<Entry>&)>;
void withDecodedArchive(const fs::path& archive, const RestoreOptions& options,
                        const DecodedArchiveConsumer& consumer);
std::string entryTypeName(EntryType type);

// ---- restore (fd based) ----
void extractEntries(const fs::path& packed, const fs::path& destination,
                    std::vector<Entry> entries, const RestoreOptions& options,
                    uint64_t& outputBytes);

} // namespace detail
} // namespace backup
