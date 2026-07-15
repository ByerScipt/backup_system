#ifndef BACKUP_CORE_HPP
#define BACKUP_CORE_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace backup {

enum class PackAlgorithm : uint8_t { Stream = 1, Index = 2 };
enum class CompressionAlgorithm : uint8_t { None = 0, Rle = 1, Huffman = 2 };
enum class EncryptionAlgorithm : uint8_t { None = 0, Xor = 1, Vigenere = 2 };

struct ProgressEvent {
    std::string stage;
    uint64_t completed = 0;
    uint64_t total = 0;
    std::string detail;
};

using ProgressCallback = std::function<void(const ProgressEvent&)>;

struct BackupOptions {
    PackAlgorithm pack = PackAlgorithm::Stream;
    CompressionAlgorithm compression = CompressionAlgorithm::None;
    EncryptionAlgorithm encryption = EncryptionAlgorithm::None;
    std::string password;
    ProgressCallback progress;
    std::atomic_bool* cancel = nullptr;
};

struct RestoreOptions {
    std::string password;
    bool overwrite = false;
    ProgressCallback progress;
    std::atomic_bool* cancel = nullptr;
};

struct BackupResult {
    bool success = false;
    std::string message;
    uint64_t entryCount = 0;
    uint64_t inputBytes = 0;
    uint64_t outputBytes = 0;
};

struct ArchiveInfo {
    uint16_t version = 0;
    PackAlgorithm pack = PackAlgorithm::Stream;
    CompressionAlgorithm compression = CompressionAlgorithm::None;
    EncryptionAlgorithm encryption = EncryptionAlgorithm::None;
    uint64_t packedSize = 0;
    uint64_t encodedSize = 0;
    std::array<uint8_t, 32> packedDigest{};
    std::array<uint8_t, 32> encodedDigest{};
};

struct ArchiveEntryInfo {
    std::string path;
    std::string type;
    uint64_t size = 0;
};

struct RestorePreview {
    std::vector<ArchiveEntryInfo> entries;
    std::vector<std::string> conflicts;
};

class BackupEngine {
public:
    static BackupResult create(const std::string& sourceDirectory,
                               const std::string& archivePath,
                               const BackupOptions& options);

    static BackupResult restore(const std::string& archivePath,
                                const std::string& destinationDirectory,
                                const RestoreOptions& options);

    static RestorePreview preview(const std::string& archivePath,
                                  const std::string& destinationDirectory,
                                  const std::string& password = {});

    static ArchiveInfo inspect(const std::string& archivePath);
};

std::array<uint8_t, 32> sha256(const std::vector<uint8_t>& data);
std::array<uint8_t, 32> sha256(const std::string& data);
std::array<uint8_t, 32> sha256File(const std::string& path,
                                  uint64_t offset = 0,
                                  uint64_t length = UINT64_MAX);
std::string hexDigest(const std::array<uint8_t, 32>& digest);

std::string toString(PackAlgorithm value);
std::string toString(CompressionAlgorithm value);
std::string toString(EncryptionAlgorithm value);
PackAlgorithm parsePackAlgorithm(const std::string& value);
CompressionAlgorithm parseCompressionAlgorithm(const std::string& value);
EncryptionAlgorithm parseEncryptionAlgorithm(const std::string& value);

} // namespace backup

#endif
