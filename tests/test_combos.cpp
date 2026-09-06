#include "helpers.hpp"



namespace fs = std::filesystem;
using namespace backup;

void testSha256() {
    check(hexDigest(sha256(std::string("abc"))) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 known vector failed");
}


std::vector<BuiltArchive> testAllCombinations(
    const fs::path& workspace, const fs::path& source,
    const std::vector<std::string>& fixturePaths) {
    std::vector<BuiltArchive> archives;
    int number = 0;
    for (auto pack : {PackAlgorithm::Stream, PackAlgorithm::Index}) {
        for (auto compression : {CompressionAlgorithm::None, CompressionAlgorithm::Rle,
                                 CompressionAlgorithm::Huffman}) {
            for (auto encryption : {EncryptionAlgorithm::None,
                                    EncryptionAlgorithm::ChaCha20,
                                    EncryptionAlgorithm::Aes256}) {
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

