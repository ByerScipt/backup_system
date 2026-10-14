#include "internal.hpp"

namespace backup::detail
{

namespace
{

// Blocks are encoded independently: the hash chain only spans the current
// block, so a match never reaches across a block boundary and peak memory
// stays bounded however large the packed stream is.
constexpr size_t kLz77BlockSize = 1024 * 1024;
constexpr size_t kLz77MinMatch = 3;
// Distances are stored in two bytes, which also fixes the sliding window.
constexpr size_t kLz77Window = 0xFFFF;
// Longer matches are emitted as consecutive sequences.
constexpr size_t kLz77MaxMatch = 0xFFFF;
// Hash-chain walk per position: the only compression-ratio trade-off.
constexpr size_t kLz77MaxChain = 64;
constexpr size_t kLz77HashBits = 16;
constexpr uint32_t kLz77NoPosition = std::numeric_limits<uint32_t>::max();
constexpr uint8_t kLz77TokenBlock = 0;
constexpr uint8_t kLz77StoredBlock = 1;
constexpr std::array<char, 4> kLz77Magic{{'L', 'Z', '7', '7'}};

struct Lz77Match
{
    size_t length = 0;
    size_t distance = 0;
};

uint32_t lz77Hash(const uint8_t* bytes)
{
    const uint32_t value = (static_cast<uint32_t>(bytes[0]) << 16) |
                           (static_cast<uint32_t>(bytes[1]) << 8) |
                           static_cast<uint32_t>(bytes[2]);
    return (value * 2654435761u) >> (32 - kLz77HashBits);
}

// Appends a 255-based length extension: values below 255 use a single byte,
// larger ones repeat 255 until the remainder fits.
void appendExtension(std::vector<uint8_t>& out, size_t extra)
{
    while (extra >= 255)
    {
        out.push_back(255);
        extra -= 255;
    }
    out.push_back(static_cast<uint8_t>(extra));
}

// A sequence is a token byte, the literals preceding the match and a two-byte
// distance. The token packs the literal count in the high nibble and the
// match length offset by kLz77MinMatch in the low nibble; a nibble of 15
// continues in extension bytes. The final sequence of a block holds literals
// only: it carries no distance, so the decoder stops as soon as the literals
// fill the declared block size.
void appendSequence(std::vector<uint8_t>& out, const uint8_t* literals,
                    size_t literalCount, size_t matchLength, uint16_t distance)
{
    const size_t matchCode = matchLength == 0 ? 0 : matchLength - kLz77MinMatch;
    out.push_back(
        static_cast<uint8_t>((std::min<size_t>(literalCount, 15) << 4) |
                             std::min<size_t>(matchCode, 15)));
    if (literalCount >= 15)
    {
        appendExtension(out, literalCount - 15);
    }
    out.insert(out.end(), literals, literals + literalCount);
    if (matchLength == 0)
    {
        return;
    }
    if (matchCode >= 15)
    {
        appendExtension(out, matchCode - 15);
    }
    out.push_back(static_cast<uint8_t>(distance & 0xFFu));
    out.push_back(static_cast<uint8_t>(distance >> 8));
}

// Greedy encoder with one step of lazy evaluation: a match at the current
// position is skipped when the next position starts a strictly longer one,
// which avoids trading a long match for a short one plus one literal.
// Positions covered by an emitted match are not inserted into the hash chain;
// the alternative (insert every position) costs a lot of time for a small
// gain on backup data.
class Lz77Encoder
{
public:
    Lz77Encoder() : head_(size_t(1) << kLz77HashBits, kLz77NoPosition) {}

    const std::vector<uint8_t>& encode(const uint8_t* data, size_t size)
    {
        output_.clear();
        if (size < kLz77MinMatch)
        {
            appendSequence(output_, data, size, 0, 0);
            return output_;
        }
        std::fill(head_.begin(), head_.end(), kLz77NoPosition);
        previous_.resize(size);
        size_t literalStart = 0;
        size_t position = 0;
        while (position + kLz77MinMatch <= size)
        {
            insert(data, position);
            const Lz77Match match = search(data, size, position);
            if (match.length < kLz77MinMatch)
            {
                ++position;
                continue;
            }
            if (position + 1 + kLz77MinMatch <= size &&
                search(data, size, position + 1).length > match.length)
            {
                ++position;
                continue;
            }
            appendSequence(output_, data + literalStart,
                           position - literalStart, match.length,
                           static_cast<uint16_t>(match.distance));
            position += match.length;
            literalStart = position;
        }
        if (literalStart < size)
        {
            appendSequence(output_, data + literalStart, size - literalStart, 0,
                           0);
        }
        return output_;
    }

private:
    // Requires three readable bytes at position, which also bounds the hash.
    void insert(const uint8_t* data, size_t position)
    {
        const uint32_t hash = lz77Hash(data + position);
        previous_[position] = head_[hash];
        head_[hash] = static_cast<uint32_t>(position);
    }

    // Candidates at or after position are skipped: the chain holds the
    // position being encoded itself once it has been inserted.
    Lz77Match search(const uint8_t* data, size_t size, size_t position) const
    {
        Lz77Match best;
        uint32_t candidate = head_[lz77Hash(data + position)];
        size_t steps = 0;
        while (candidate != kLz77NoPosition && steps < kLz77MaxChain)
        {
            ++steps;
            if (candidate < position)
            {
                const size_t distance = position - candidate;
                if (distance <= kLz77Window)
                {
                    const size_t limit =
                        std::min(size - position, kLz77MaxMatch);
                    size_t length = 0;
                    while (length < limit &&
                           data[candidate + length] == data[position + length])
                    {
                        ++length;
                    }
                    if (length > best.length)
                    {
                        best = {length, distance};
                        if (length == limit)
                        {
                            break; // a full-length match cannot be beaten
                        }
                    }
                }
            }
            candidate = previous_[candidate];
        }
        return best;
    }

    std::vector<uint8_t> output_;
    std::vector<uint32_t> previous_;
    std::vector<uint32_t> head_;
};

// Decodes one token block. Every length, offset and extension byte is checked
// against the two declared sizes, so a malformed block can neither read
// outside the buffers nor write past the declared block size.
void decodeBlock(const uint8_t* input, size_t inputSize, uint8_t* output,
                 size_t outputSize)
{
    size_t read = 0;
    size_t produced = 0;
    auto readByte = [&]() -> uint8_t
    {
        ensure(read < inputSize, "truncated LZ77 token stream");
        return input[read++];
    };
    // Extension bytes accumulate a value that both literal runs and match
    // lengths bound by the block size, so the cap also ends the loop.
    auto readLength = [&](size_t base) -> size_t
    {
        size_t value = base;
        for (;;)
        {
            ensure(value <= outputSize, "LZ77 length exceeds block size");
            const uint8_t byte = readByte();
            ensure(value + byte <= outputSize,
                   "LZ77 length exceeds block size");
            value += byte;
            if (byte != 255)
            {
                return value;
            }
        }
    };

    while (true)
    {
        if (produced == outputSize)
        {
            ensure(read == inputSize, "LZ77 block has trailing data");
            return;
        }
        const uint8_t token = readByte();
        size_t literalCount = token >> 4;
        if (literalCount == 15)
        {
            literalCount = readLength(15);
        }
        ensure(literalCount <= inputSize - read, "truncated LZ77 literal run");
        ensure(literalCount <= outputSize - produced,
               "LZ77 literal run exceeds block size");
        std::memcpy(output + produced, input + read, literalCount);
        read += literalCount;
        produced += literalCount;
        if (produced == outputSize)
        {
            ensure(read == inputSize, "LZ77 block has trailing data");
            return;
        }

        size_t matchLength = token & 0x0Fu;
        if (matchLength == 15)
        {
            matchLength = readLength(15);
        }
        matchLength += kLz77MinMatch;
        ensure(inputSize - read >= 2, "truncated LZ77 match distance");
        const size_t distance = static_cast<size_t>(input[read]) |
                                (static_cast<size_t>(input[read + 1]) << 8);
        read += 2;
        // The distance must address bytes that were already produced, which
        // also proves that the copy stays inside the block.
        ensure(distance != 0 && distance <= produced,
               "invalid LZ77 match distance");
        ensure(matchLength <= outputSize - produced,
               "LZ77 match exceeds block size");
        for (size_t index = 0; index < matchLength; ++index)
        {
            // Byte-wise copy: match source and destination may overlap.
            output[produced] = output[produced - distance];
            ++produced;
        }
    }
}

} // namespace

void lz77Compress(const fs::path& input, const fs::path& output,
                  const BackupOptions& options)
{
    std::ifstream in(input, std::ios::binary);
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    ensure(in.good() && out.good(), "cannot open LZ77 stage");
    const uint64_t originalSize = fileSizeChecked(input);
    writeExact(out, kLz77Magic.data(), kLz77Magic.size());
    writeU64(out, originalSize);

    Lz77Encoder encoder;
    std::vector<uint8_t> raw(kLz77BlockSize);
    uint64_t completed = 0;
    while (completed < originalSize)
    {
        checkCancelled(options.cancel);
        const size_t wanted = static_cast<size_t>(
            std::min<uint64_t>(raw.size(), originalSize - completed));
        in.read(reinterpret_cast<char*>(raw.data()),
                static_cast<std::streamsize>(wanted));
        ensure(static_cast<size_t>(in.gcount()) == wanted,
               "cannot read complete LZ77 input");
        const std::vector<uint8_t>& encoded =
            encoder.encode(raw.data(), wanted);
        // A block is stored verbatim whenever the token stream is not
        // smaller, so compression can never make the packed data grow.
        const bool stored = encoded.size() >= wanted;
        writeU8(out, stored ? kLz77StoredBlock : kLz77TokenBlock);
        writeU32(out, static_cast<uint32_t>(wanted));
        writeU32(out, static_cast<uint32_t>(stored ? wanted : encoded.size()));
        writeExact(out, stored ? raw.data() : encoded.data(),
                   stored ? wanted : encoded.size());
        completed += wanted;
        report(options.progress, "compress-lz77", completed, originalSize);
    }
    ensure(!in.bad(), "cannot read complete LZ77 input");
    closeOutput(out);
}

void lz77Decompress(const fs::path& input, const fs::path& output,
                    uint64_t expectedSize, const RestoreOptions& options)
{
    std::ifstream in(input, std::ios::binary);
    ensure(in.good(), "cannot open LZ77 restore input");
    std::array<char, 4> magic{};
    readExact(in, magic.data(), magic.size());
    ensure(magic == kLz77Magic, "invalid LZ77 header");
    ensure(readU64(in) == expectedSize,
           "LZ77 declared output size does not match archive header");

    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    ensure(out.good(), "cannot create LZ77 restore output");
    std::vector<uint8_t> raw;
    std::vector<uint8_t> encoded;
    uint64_t produced = 0;
    while (produced < expectedSize)
    {
        checkCancelled(options.cancel);
        const uint8_t flags = readU8(in);
        const uint32_t rawLength = readU32(in);
        const uint32_t encodedLength = readU32(in);
        ensure(flags == kLz77TokenBlock || flags == kLz77StoredBlock,
               "invalid LZ77 block flags");
        ensure(rawLength > 0 && rawLength <= kLz77BlockSize,
               "invalid LZ77 block size");
        ensure(rawLength <= expectedSize - produced,
               "LZ77 block exceeds output size limit");
        // A token block is only accepted when it is smaller than the raw
        // block; anything else must be stored. This bounds the payload
        // allocation below by the block size.
        ensure(flags == kLz77StoredBlock ? encodedLength == rawLength
                                         : encodedLength < rawLength,
               "invalid LZ77 block payload size");
        raw.resize(rawLength);
        if (flags == kLz77StoredBlock)
        {
            readExact(in, raw.data(), rawLength);
        }
        else
        {
            encoded.resize(encodedLength);
            readExact(in, encoded.data(), encodedLength);
            decodeBlock(encoded.data(), encodedLength, raw.data(), rawLength);
        }
        writeExact(out, raw.data(), rawLength);
        produced += rawLength;
        report(options.progress, "decompress-lz77", produced, expectedSize);
    }
    ensure(in.peek() == std::char_traits<char>::eof() && !in.bad(),
           "LZ77 stream has trailing data");
    closeOutput(out);
}

} // namespace backup::detail
