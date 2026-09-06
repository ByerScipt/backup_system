#include "core_internal.hpp"

#include <cassert>

namespace backup {
namespace detail {

struct HuffNode {
    uint64_t frequency = 0;
    int symbol = -1;
    int minimumSymbol = 0;
    int left = -1;
    int right = -1;
};

std::array<uint8_t, 256> buildHuffmanLengths(const std::array<uint64_t, 256>& frequencies) {
    std::vector<HuffNode> nodes;
    struct Compare {
        const std::vector<HuffNode>* nodes = nullptr;
        bool operator()(int a, int b) const {
            const auto& x = (*nodes)[a]; const auto& y = (*nodes)[b];
            if (x.frequency != y.frequency) return x.frequency > y.frequency;
            return x.minimumSymbol > y.minimumSymbol;
        }
    };
    Compare compare{&nodes};
    std::priority_queue<int, std::vector<int>, Compare> queue(compare);
    for (int symbol = 0; symbol < 256; ++symbol) {
        if (frequencies[symbol]) {
            nodes.push_back({frequencies[symbol], symbol, symbol, -1, -1});
            queue.push(static_cast<int>(nodes.size() - 1));
        }
    }
    std::array<uint8_t, 256> lengths{};
    if (queue.empty()) return lengths;
    if (queue.size() == 1) {
        lengths[nodes[queue.top()].symbol] = 1;
        return lengths;
    }
    while (queue.size() > 1) {
        int left = queue.top(); queue.pop();
        int right = queue.top(); queue.pop();
        uint64_t sum = nodes[left].frequency + nodes[right].frequency;
        // Frequencies count input bytes; total cannot exceed 2^64 in practice.
        nodes.push_back({sum, -1, std::min(nodes[left].minimumSymbol, nodes[right].minimumSymbol), left, right});
        queue.push(static_cast<int>(nodes.size() - 1));
    }
    std::function<void(int, int)> visit = [&](int index, int depth) {
        assert(depth <= 255);  // internal invariant: at most 256 symbols
        const auto& node = nodes[index];
        if (node.symbol >= 0) {
            lengths[node.symbol] = static_cast<uint8_t>(std::max(depth, 1));
            return;
        }
        visit(node.left, depth + 1);
        visit(node.right, depth + 1);
    };
    visit(queue.top(), 0);
    return lengths;
}

using HuffCode = std::vector<uint8_t>;

std::array<HuffCode, 256> canonicalCodes(const std::array<uint8_t, 256>& lengths) {
    std::vector<std::pair<uint8_t, int>> ordered;
    for (int symbol = 0; symbol < 256; ++symbol) {
        if (lengths[symbol]) ordered.emplace_back(lengths[symbol], symbol);
    }
    std::sort(ordered.begin(), ordered.end());
    std::array<HuffCode, 256> codes;
    if (ordered.empty()) return codes;
    HuffCode current(ordered.front().first, 0);
    uint8_t previousLength = ordered.front().first;
    for (size_t i = 0; i < ordered.size(); ++i) {
        uint8_t length = ordered[i].first;
        ensure(length >= previousLength, "invalid canonical Huffman order");
        if (length > previousLength) current.insert(current.end(), length - previousLength, 0);
        ensure(current.size() == length, "invalid canonical Huffman length");
        codes[ordered[i].second] = current;
        previousLength = length;
        if (i + 1 < ordered.size()) {
            bool carry = true;
            for (size_t p = current.size(); p > 0 && carry; --p) {
                if (current[p - 1] == 0) { current[p - 1] = 1; carry = false; }
                else current[p - 1] = 0;
            }
            ensure(!carry, "oversubscribed Huffman code lengths");
        }
    }
    return codes;
}

class BitWriter {
public:
    explicit BitWriter(std::ostream& out) : out_(out) {}
    void write(const HuffCode& bits) {
        for (uint8_t bit : bits) {
            current_ = static_cast<uint8_t>((current_ << 1) | bit);
            if (++used_ == 8) flushByte();
        }
    }
    void finish() {
        if (used_) {
            current_ <<= static_cast<uint8_t>(8 - used_);
            flushByte();
        }
    }
private:
    std::ostream& out_;
    uint8_t current_ = 0;
    uint8_t used_ = 0;
    void flushByte() { writeU8(out_, current_); current_ = 0; used_ = 0; }
};

class BitReader {
public:
    explicit BitReader(std::istream& in) : in_(in) {}
    int read() {
        if (remaining_ == 0) {
            int value = in_.get();
            if (value == std::char_traits<char>::eof()) return -1;
            current_ = static_cast<uint8_t>(value);
            remaining_ = 8;
        }
        int bit = (current_ >> 7) & 1;
        current_ <<= 1;
        --remaining_;
        return bit;
    }
    bool hasOnlyZeroPaddingAndEof() {
        return current_ == 0 && in_.peek() == std::char_traits<char>::eof();
    }
private:
    std::istream& in_;
    uint8_t current_ = 0;
    uint8_t remaining_ = 0;
};

void huffmanCompress(const fs::path& input, const fs::path& output,
                     const BackupOptions& options) {
    std::array<uint64_t, 256> frequencies{};
    uint64_t originalSize = fileSizeChecked(input);
    std::ifstream first(input, std::ios::binary);
    ensure(first.good(), "cannot open Huffman input");
    std::vector<uint8_t> buffer(kBufferSize);
    uint64_t counted = 0;
    while (first) {
        checkCancelled(options.cancel);
        first.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        size_t got = static_cast<size_t>(first.gcount());
        for (size_t i = 0; i < got; ++i) ++frequencies[buffer[i]];  // 64-bit counter cannot overflow
        counted += got;
        report(options.progress, "huffman-count", counted, originalSize);
    }
    auto lengths = buildHuffmanLengths(frequencies);
    auto codes = canonicalCodes(lengths);

    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    ensure(out.good(), "cannot create Huffman stage");
    const std::array<char,4> magic{{'H','U','F','1'}};
    writeExact(out, magic.data(), magic.size());
    writeU64(out, originalSize);
    writeExact(out, lengths.data(), lengths.size());

    std::ifstream second(input, std::ios::binary);
    ensure(second.good(), "cannot reopen Huffman input");
    BitWriter writer(out);
    uint64_t completed = 0;
    while (second) {
        checkCancelled(options.cancel);
        second.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        size_t got = static_cast<size_t>(second.gcount());
        for (size_t i = 0; i < got; ++i) writer.write(codes[buffer[i]]);
        completed += got;
        report(options.progress, "compress-huffman", completed, originalSize);
    }
    writer.finish();
}

void huffmanDecompress(const fs::path& input, const fs::path& output,
                       uint64_t expectedSize, const RestoreOptions& options) {
    std::ifstream in(input, std::ios::binary);
    ensure(in.good(), "cannot open Huffman restore input");
    std::array<char,4> magic{}; readExact(in, magic.data(), magic.size());
    ensure(magic == std::array<char,4>{{'H','U','F','1'}}, "invalid Huffman header");
    uint64_t originalSize = readU64(in);
    ensure(originalSize == expectedSize,
           "Huffman declared output size does not match archive header");
    std::array<uint8_t,256> lengths{}; readExact(in, lengths.data(), lengths.size());
    auto codes = canonicalCodes(lengths);
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    ensure(out.good(), "cannot create Huffman restore output");

    struct TrieNode { int child[2]{-1,-1}; int symbol = -1; };
    std::vector<TrieNode> trie(1);
    size_t symbolCount = 0;
    for (int symbol = 0; symbol < 256; ++symbol) {
        if (codes[symbol].empty()) continue;
        ++symbolCount;
        int node = 0;
        for (uint8_t bit : codes[symbol]) {
            ensure(trie[node].symbol < 0, "invalid Huffman prefix table");
            if (trie[node].child[bit] < 0) {
                trie[node].child[bit] = static_cast<int>(trie.size());
                trie.emplace_back();
            }
            node = trie[node].child[bit];
        }
        ensure(trie[node].symbol < 0 && trie[node].child[0] < 0 && trie[node].child[1] < 0,
               "duplicate or non-prefix-free Huffman table");
        trie[node].symbol = symbol;
    }
    ensure((originalSize == 0 && symbolCount == 0) || (originalSize > 0 && symbolCount > 0),
           "Huffman symbol table does not match declared size");
    if (originalSize == 0) {
        ensure(in.peek() == std::char_traits<char>::eof(), "empty Huffman stream has trailing data");
        return;
    }
    BitReader reader(in);
    std::vector<uint8_t> outputBuffer;
    outputBuffer.reserve(kBufferSize);
    uint64_t produced = 0;
    int node = 0;
    while (produced < originalSize) {
        checkCancelled(options.cancel);
        int bit = reader.read();
        ensure(bit >= 0, "truncated Huffman bitstream");
        node = trie[node].child[bit];
        ensure(node >= 0, "invalid Huffman bitstream");
        if (trie[node].symbol >= 0) {
            outputBuffer.push_back(static_cast<uint8_t>(trie[node].symbol));
            ++produced;
            node = 0;
            if (outputBuffer.size() == kBufferSize) {
                writeExact(out, outputBuffer.data(), outputBuffer.size());
                outputBuffer.clear();
                report(options.progress, "decompress-huffman", produced, originalSize);
            }
        }
    }
    if (!outputBuffer.empty()) writeExact(out, outputBuffer.data(), outputBuffer.size());
    report(options.progress, "decompress-huffman", produced, originalSize);
    ensure(reader.hasOnlyZeroPaddingAndEof(), "Huffman stream has non-zero padding or trailing data");
}
} // namespace detail
} // namespace backup
