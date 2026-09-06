#include "core_internal.hpp"

namespace backup {
namespace detail {

uint64_t fileSizeChecked(const fs::path& path) {
    std::error_code ec;
    uint64_t size = static_cast<uint64_t>(fs::file_size(path, ec));
    ensure(!ec, "cannot determine file size: " + path.string());
    return size;
}

void copyFileStage(const fs::path& input, const fs::path& output,
                   const std::string& stage, const ProgressCallback& progress,
                   std::atomic_bool* cancel) {
    std::ifstream in(input, std::ios::binary);
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    ensure(in.good() && out.good(), "cannot open pipeline stage");
    copyBytes(in, out, fileSizeChecked(input), progress, stage, cancel);
}

void compressStage(const fs::path& input, const fs::path& output,
                   CompressionAlgorithm algorithm, const BackupOptions& options) {
    switch (algorithm) {
    case CompressionAlgorithm::None:
        copyFileStage(input, output, "compress-copy", options.progress, options.cancel); break;
    case CompressionAlgorithm::Rle:
        rleCompress(input, output, options); break;
    case CompressionAlgorithm::Huffman:
        huffmanCompress(input, output, options); break;
    default: throw BackupError("unsupported compression algorithm");
    }
}

void decompressStage(const fs::path& input, const fs::path& output,
                     CompressionAlgorithm algorithm, uint64_t expectedSize,
                     const RestoreOptions& options) {
    switch (algorithm) {
    case CompressionAlgorithm::None:
        ensure(fileSizeChecked(input) == expectedSize,
               "decoded packed size mismatch (wrong password or damaged archive)");
        copyFileStage(input, output, "decompress-copy", options.progress, options.cancel); break;
    case CompressionAlgorithm::Rle:
        rleDecompress(input, output, expectedSize, options); break;
    case CompressionAlgorithm::Huffman:
        huffmanDecompress(input, output, expectedSize, options); break;
    default: throw BackupError("unsupported compression algorithm");
    }
}

void rleCompress(const fs::path& input, const fs::path& output,
                 const BackupOptions& options) {
    std::ifstream in(input, std::ios::binary);
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    ensure(in.good() && out.good(), "cannot open RLE stage");
    const std::array<char, 4> magic{{'R','L','E','1'}};
    writeExact(out, magic.data(), magic.size());
    uint64_t originalSize = fileSizeChecked(input);
    writeU64(out, originalSize);

    std::vector<uint8_t> literals;
    literals.reserve(128);
    auto flushLiterals = [&]() {
        if (literals.empty()) return;
        writeU8(out, static_cast<uint8_t>(literals.size() - 1));
        writeExact(out, literals.data(), literals.size());
        literals.clear();
    };
    auto addLiteral = [&](uint8_t value) {
        literals.push_back(value);
        if (literals.size() == 128) flushLiterals();
    };
    auto flushRun = [&](uint8_t value, size_t count) {
        if (count >= 3) {
            flushLiterals();
            writeU8(out, static_cast<uint8_t>(0x80u | (count - 1)));
            writeU8(out, value);
        } else {
            for (size_t i = 0; i < count; ++i) addLiteral(value);
        }
    };

    std::vector<uint8_t> buffer(kBufferSize);
    bool haveRun = false;
    uint8_t runByte = 0;
    size_t runLength = 0;
    uint64_t completed = 0;
    while (in) {
        checkCancelled(options.cancel);
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        size_t got = static_cast<size_t>(in.gcount());
        for (size_t i = 0; i < got; ++i) {
            uint8_t value = buffer[i];
            if (!haveRun) {
                haveRun = true; runByte = value; runLength = 1;
            } else if (value == runByte && runLength < 128) {
                ++runLength;
            } else {
                flushRun(runByte, runLength);
                runByte = value; runLength = 1;
            }
        }
        completed += got;
        report(options.progress, "compress-rle", completed, originalSize);
    }
    if (haveRun) flushRun(runByte, runLength);
    flushLiterals();
}

void rleDecompress(const fs::path& input, const fs::path& output,
                   uint64_t expectedSize, const RestoreOptions& options) {
    std::ifstream in(input, std::ios::binary);
    ensure(in.good(), "cannot open RLE restore input");
    std::array<char, 4> magic{}; readExact(in, magic.data(), magic.size());
    ensure(magic == std::array<char,4>{{'R','L','E','1'}}, "invalid RLE header");
    uint64_t originalSize = readU64(in);
    ensure(originalSize == expectedSize,
           "RLE declared output size does not match archive header");
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    ensure(out.good(), "cannot create RLE restore output");
    uint64_t produced = 0;
    while (produced < originalSize) {
        checkCancelled(options.cancel);
        uint8_t control = readU8(in);
        size_t count = static_cast<size_t>(control & 0x7fu) + 1;
        ensure(count <= expectedSize - produced, "RLE block exceeds output size limit");
        if (control & 0x80u) {
            uint8_t value = readU8(in);
            std::array<uint8_t, 128> repeated{};
            repeated.fill(value);
            writeExact(out, repeated.data(), count);
        } else {
            std::array<uint8_t, 128> literal{};
            readExact(in, literal.data(), count);
            writeExact(out, literal.data(), count);
        }
        produced += count;
        report(options.progress, "decompress-rle", produced, originalSize);
    }
    ensure(in.peek() == std::char_traits<char>::eof(), "RLE stream has trailing data");
}

}  // namespace detail
}  // namespace backup
