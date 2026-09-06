#include "core_internal.hpp"

namespace backup {
namespace detail {

std::array<uint8_t, 32> deriveKey(const std::string& password,
                                  const std::array<uint8_t,16>& salt) {
    std::vector<uint8_t> material(password.begin(), password.end());
    material.insert(material.end(), salt.begin(), salt.end());
    auto digest = sha256(material);
    for (int round = 0; round < 4096; ++round) {
        std::vector<uint8_t> next(digest.begin(), digest.end());
        next.insert(next.end(), salt.begin(), salt.end());
        digest = sha256(next);
    }
    return digest;
}

class XorGenerator {
public:
    explicit XorGenerator(const std::array<uint8_t,32>& key) {
        for (int i = 0; i < 8; ++i) {
            a_ |= static_cast<uint64_t>(key[i]) << (i * 8);
            b_ |= static_cast<uint64_t>(key[i + 8]) << (i * 8);
        }
        if (a_ == 0 && b_ == 0) b_ = 0x9e3779b97f4a7c15ULL;
    }
    uint8_t next() {
        if (available_ == 0) {
            uint64_t x = a_, y = b_;
            a_ = y;
            x ^= x << 23;
            b_ = x ^ y ^ (x >> 17) ^ (y >> 26);
            block_ = b_ + y;
            available_ = 8;
        }
        uint8_t value = static_cast<uint8_t>(block_);
        block_ >>= 8;
        --available_;
        return value;
    }
private:
    uint64_t a_ = 0, b_ = 0, block_ = 0;
    int available_ = 0;
};

void cryptStage(const fs::path& input, const fs::path& output,
                EncryptionAlgorithm algorithm, const std::string& password,
                const std::array<uint8_t,16>& salt,
                const ProgressCallback& progress, std::atomic_bool* cancel,
                const std::string& stage) {
    ensure(algorithm == EncryptionAlgorithm::None || !password.empty(),
           "an encryption password is required");
    std::ifstream in(input, std::ios::binary);
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    ensure(in.good() && out.good(), "cannot open encryption pipeline stage");
    uint64_t total = fileSizeChecked(input), completed = 0, position = 0;
    auto key = deriveKey(password, salt);
    XorGenerator generator(key);
    std::vector<uint8_t> buffer(kBufferSize);
    while (in) {
        checkCancelled(cancel);
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        size_t got = static_cast<size_t>(in.gcount());
        if (algorithm == EncryptionAlgorithm::Xor) {
            for (size_t i = 0; i < got; ++i) buffer[i] ^= generator.next();
        } else if (algorithm == EncryptionAlgorithm::Vigenere) {
            for (size_t i = 0; i < got; ++i) {
                uint8_t k = key[(position + i) % key.size()];
                if (stage == "encrypt") buffer[i] = static_cast<uint8_t>(buffer[i] + k);
                else buffer[i] = static_cast<uint8_t>(buffer[i] - k);
            }
        } else if (algorithm != EncryptionAlgorithm::None) {
            throw BackupError("unsupported encryption algorithm");
        }
        if (got) writeExact(out, buffer.data(), got);
        position += got;
        completed += got;
        report(progress, stage, completed, total);
    }
}

std::array<uint8_t,16> randomSalt() {
    std::array<uint8_t,16> salt{};
    std::random_device random;
    for (auto& b : salt) b = static_cast<uint8_t>(random());
    return salt;
}
} // namespace detail
} // namespace backup
