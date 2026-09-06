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

namespace {

// ---- ChaCha20 (RFC 8439, IETF variant) ----
// Self-contained (no third-party code) so the course extension keeps full credit.

inline uint32_t rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

inline void quarterRound(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    a += b; d ^= a; d = rotl32(d, 16);
    c += d; b ^= c; b = rotl32(b, 12);
    a += b; d ^= a; d = rotl32(d, 8);
    c += d; b ^= c; b = rotl32(b, 7);
}

inline uint32_t load32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void chacha20Block(const std::array<uint8_t,32>& key,
                   const std::array<uint8_t,12>& nonce,
                   uint32_t counter, uint8_t out[64]) {
    static constexpr uint32_t kConstants[4] = {0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u};
    uint32_t state[16];
    state[0] = kConstants[0]; state[1] = kConstants[1];
    state[2] = kConstants[2]; state[3] = kConstants[3];
    for (int i = 0; i < 8; ++i) state[4 + i] = load32le(key.data() + i * 4);
    state[12] = counter;
    for (int i = 0; i < 3; ++i) state[13 + i] = load32le(nonce.data() + i * 4);
    uint32_t working[16];
    std::memcpy(working, state, sizeof(state));
    for (int i = 0; i < 10; ++i) {
        quarterRound(working[0], working[4], working[8], working[12]);
        quarterRound(working[1], working[5], working[9], working[13]);
        quarterRound(working[2], working[6], working[10], working[14]);
        quarterRound(working[3], working[7], working[11], working[15]);
        quarterRound(working[0], working[5], working[10], working[15]);
        quarterRound(working[1], working[6], working[11], working[12]);
        quarterRound(working[2], working[7], working[8], working[13]);
        quarterRound(working[3], working[4], working[9], working[14]);
    }
    for (int i = 0; i < 16; ++i) {
        uint32_t word = working[i] + state[i];
        out[i * 4] = static_cast<uint8_t>(word);
        out[i * 4 + 1] = static_cast<uint8_t>(word >> 8);
        out[i * 4 + 2] = static_cast<uint8_t>(word >> 16);
        out[i * 4 + 3] = static_cast<uint8_t>(word >> 24);
    }
}

// ---- AES-256 (FIPS 197) ----

const uint8_t kAesSBox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

inline uint8_t xtime(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00)); }

void aes256ExpandKey(const uint8_t key[32], uint8_t roundKeys[240]) {
    static constexpr uint8_t kRcon[8] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40};
    std::memcpy(roundKeys, key, 32);
    for (int i = 8; i < 60; ++i) {
        uint8_t temp[4];
        std::memcpy(temp, roundKeys + (i - 1) * 4, 4);
        if (i % 8 == 0) {
            uint8_t first = temp[0];
            temp[0] = temp[1]; temp[1] = temp[2]; temp[2] = temp[3]; temp[3] = first;
            for (int j = 0; j < 4; ++j) temp[j] = kAesSBox[temp[j]];
            temp[0] ^= kRcon[i / 8];
        } else if (i % 8 == 4) {
            for (int j = 0; j < 4; ++j) temp[j] = kAesSBox[temp[j]];
        }
        for (int j = 0; j < 4; ++j) roundKeys[i * 4 + j] = roundKeys[(i - 8) * 4 + j] ^ temp[j];
    }
}

inline void aesAddRoundKey(uint8_t state[16], const uint8_t roundKeys[240], int round) {
    for (int i = 0; i < 16; ++i) state[i] ^= roundKeys[round * 16 + i];
}

inline void aesSubBytes(uint8_t state[16]) {
    for (int i = 0; i < 16; ++i) state[i] = kAesSBox[state[i]];
}

inline void aesShiftRows(uint8_t state[16]) {
    uint8_t temp[16];
    std::memcpy(temp, state, 16);
    state[1] = temp[5]; state[5] = temp[9]; state[9] = temp[13]; state[13] = temp[1];
    state[2] = temp[10]; state[6] = temp[14]; state[10] = temp[2]; state[14] = temp[6];
    state[3] = temp[15]; state[7] = temp[3]; state[11] = temp[7]; state[15] = temp[11];
}

inline void aesMixColumns(uint8_t state[16]) {
    for (int col = 0; col < 4; ++col) {
        uint8_t a0 = state[col * 4], a1 = state[col * 4 + 1];
        uint8_t a2 = state[col * 4 + 2], a3 = state[col * 4 + 3];
        state[col * 4] = static_cast<uint8_t>(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
        state[col * 4 + 1] = static_cast<uint8_t>(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
        state[col * 4 + 2] = static_cast<uint8_t>(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
        state[col * 4 + 3] = static_cast<uint8_t>((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
    }
}

void aes256EncryptBlock(const uint8_t roundKeys[240], const uint8_t in[16], uint8_t out[16]) {
    uint8_t state[16];
    std::memcpy(state, in, 16);
    aesAddRoundKey(state, roundKeys, 0);
    for (int round = 1; round < 14; ++round) {
        aesSubBytes(state);
        aesShiftRows(state);
        aesMixColumns(state);
        aesAddRoundKey(state, roundKeys, round);
    }
    aesSubBytes(state);
    aesShiftRows(state);
    aesAddRoundKey(state, roundKeys, 14);
    std::memcpy(out, state, 16);
}

} // namespace

// XOR [position, position + size) with the ChaCha20 keystream.
// Encrypt and decrypt are the same operation.
void chacha20Xor(const std::array<uint8_t,32>& key,
                 const std::array<uint8_t,12>& nonce,
                 uint64_t position, uint8_t* data, size_t size) {
    uint64_t blockIndex = position / 64;
    size_t offset = static_cast<size_t>(position % 64);
    size_t done = 0;
    while (done < size) {
        ensure(blockIndex <= UINT32_MAX, "archive exceeds the ChaCha20 counter range");
        uint8_t block[64];
        chacha20Block(key, nonce, static_cast<uint32_t>(blockIndex), block);
        size_t take = std::min(size - done, 64 - offset);
        for (size_t i = 0; i < take; ++i) data[done + i] ^= block[offset + i];
        done += take;
        ++blockIndex;
        offset = 0;
    }
}

// XOR with AES-256-CTR keystream; the 16-byte salt is the initial counter block.
void aes256CtrXor(const std::array<uint8_t,32>& key,
                  const std::array<uint8_t,16>& counter,
                  uint64_t position, uint8_t* data, size_t size) {
    uint8_t roundKeys[240];
    aes256ExpandKey(key.data(), roundKeys);
    uint64_t blockIndex = position / 16;
    size_t offset = static_cast<size_t>(position % 16);
    size_t done = 0;
    while (done < size) {
        uint8_t block[16];
        std::memcpy(block, counter.data(), 16);
        uint64_t carry = blockIndex;
        for (int i = 15; i >= 0 && carry; --i) {
            uint64_t sum = static_cast<uint64_t>(block[i]) + (carry & 0xff);
            block[i] = static_cast<uint8_t>(sum);
            carry = (carry >> 8) + (sum >> 8);
        }
        uint8_t keystream[16];
        aes256EncryptBlock(roundKeys, block, keystream);
        size_t take = std::min(size - done, 16 - offset);
        for (size_t i = 0; i < take; ++i) data[done + i] ^= keystream[offset + i];
        done += take;
        ++blockIndex;
        offset = 0;
    }
}

void cryptStage(const fs::path& input, const fs::path& output,
                EncryptionAlgorithm algorithm, const std::string& password,
                const std::array<uint8_t,16>& salt,
                const ProgressCallback& progress, std::atomic_bool* cancel,
                const std::string& stage) {
    ensure(algorithm == EncryptionAlgorithm::None || !password.empty(),
           "an encryption password is required");
    ensure(algorithm == EncryptionAlgorithm::None ||
           algorithm == EncryptionAlgorithm::ChaCha20 ||
           algorithm == EncryptionAlgorithm::Aes256,
           "unsupported encryption algorithm");
    static_cast<void>(stage);
    std::ifstream in(input, std::ios::binary);
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    ensure(in.good() && out.good(), "cannot open encryption pipeline stage");
    uint64_t total = fileSizeChecked(input), completed = 0, position = 0;
    if (algorithm == EncryptionAlgorithm::None) {
        copyBytes(in, out, total, progress, "crypt-copy", cancel);
        return;
    }
    auto key = deriveKey(password, salt);
    std::vector<uint8_t> buffer(kBufferSize);
    const char* stageName = algorithm == EncryptionAlgorithm::ChaCha20
        ? "crypt-chacha20" : "crypt-aes256";
    if (algorithm == EncryptionAlgorithm::ChaCha20) {
        // 32-bit block counter covers 256 GiB per archive; stay well inside it.
        ensure(total < (1ULL << 38), "archive exceeds the ChaCha20 size limit");
        std::array<uint8_t,12> nonce{};
        std::copy(salt.begin(), salt.begin() + 12, nonce.begin());
        while (in) {
            checkCancelled(cancel);
            in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            size_t got = static_cast<size_t>(in.gcount());
            if (got) {
                chacha20Xor(key, nonce, position, buffer.data(), got);
                writeExact(out, buffer.data(), got);
            }
            position += got;
            completed += got;
            report(progress, stageName, completed, total);
        }
        return;
    }
    while (in) {
        checkCancelled(cancel);
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        size_t got = static_cast<size_t>(in.gcount());
        if (got) {
            aes256CtrXor(key, salt, position, buffer.data(), got);
            writeExact(out, buffer.data(), got);
        }
        position += got;
        completed += got;
        report(progress, stageName, completed, total);
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
