#include "helpers.hpp"

#include "core/internal.hpp"

#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace backup;

namespace {

std::vector<uint8_t> fromHex(const std::string& hex) {
    check(hex.size() % 2 == 0, "odd-length test vector");
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

std::array<uint8_t, 16> fromHexToArray16(const std::string& hex) {
    auto bytes = fromHex(hex);
    check(bytes.size() == 16, "test counter must be 16 bytes");
    std::array<uint8_t, 16> out{};
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

std::string toHex(const std::vector<uint8_t>& data) {
    std::ostringstream out;
    out << std::hex;
    for (uint8_t b : data) {
        out.width(2);
        out.fill('0');
        out << static_cast<unsigned>(b);
    }
    return out.str();
}

} // namespace

// Known-answer tests for the two self-implemented ciphers.
// The counter-1 ChaCha20 keystream starts 10f1e7e4…, anchoring us to RFC 8439;
// every value below was cross-checked against an independent implementation
// (plus openssl for the AES-256-ECB FIPS C.3 line) before being frozen here.
void testCryptoVectors() {
    std::array<uint8_t, 32> key{};
    for (int i = 0; i < 32; ++i)
        key[i] = static_cast<uint8_t>(i);
    auto nonceBytes = fromHex("000000090000004a00000000");
    std::array<uint8_t, 12> nonce{};
    std::copy(nonceBytes.begin(), nonceBytes.end(), nonce.begin());

    std::vector<uint8_t> zeros(128, 0);
    detail::chacha20Xor(key, nonce, 0, zeros.data(), zeros.size());
    check(toHex(zeros) == "8adc91fd9ff4f0f51b0fad50ff15d637e40efda206cc52c783a74200503c15"
                          "82cd9833367d0a54d57d3c9e998f490ee69ca34c1ff9e939a75584c52d690a35d4"
                          "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
                          "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e",
          "chacha20 block vectors failed");

    // Offset handling: a mid-stream window must match the same keystream slice.
    std::vector<uint8_t> window(100, 0);
    detail::chacha20Xor(key, nonce, 5, window.data(), window.size());
    check(std::vector<uint8_t>(zeros.begin() + 5, zeros.begin() + 105) == window,
          "chacha20 stream offset failed");

    std::string text =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip "
        "for the future, sunscreen would be it.";
    std::vector<uint8_t> message(text.begin(), text.end());
    detail::chacha20Xor(key, nonce, 0, message.data(), message.size());
    check(toHex(message) == "c6bdf594fa87d094756b8d179a7ba25b816398cc26a334e7f7cf2720335074f1"
                            "beb85c505d2d6dec471cd7ffaf002e85f3d6207bd9865fc130f6e554067f15bb7"
                            "e9d9ec4be553c352466ad3fc54f03e4b3b991e755b51c76764786bab0a1023db1"
                            "f0012369bfdd6661aeb325bbee22cbc13c",
          "chacha20 message vector failed");

    // AES-256-CTR rides on the ECB block cipher; the FIPS-197 C.3 line pins it.
    std::vector<uint8_t> fips(16, 0);
    detail::aes256CtrXor(key, fromHexToArray16("00112233445566778899aabbccddeeff"), 0, fips.data(),
                         fips.size());
    check(toHex(fips) == "8ea2b7ca516745bfeafc49904b496089", "aes-256 FIPS C.3 vector failed");

    std::array<uint8_t, 16> zeroCounter{};
    std::vector<uint8_t> ctr(64, 0);
    detail::aes256CtrXor(key, zeroCounter, 0, ctr.data(), ctr.size());
    check(toHex(ctr) == "f29000b62a499fd0a9f39a6add2e7780"
                        "f05d76ae4ab99fe5a6f69b3148c2363d"
                        "0ebcb5deb52c83bd08a8a935182c9199"
                        "d24356532881602f809eb383c5ff5d56",
          "aes-256-ctr blocks 0-3 failed");

    // Same offset rule for the CTR stream.
    std::vector<uint8_t> ctrWindow(37, 0);
    detail::aes256CtrXor(key, zeroCounter, 7, ctrWindow.data(), ctrWindow.size());
    check(std::vector<uint8_t>(ctr.begin() + 7, ctr.begin() + 44) == ctrWindow,
          "aes-256-ctr stream offset failed");
}
