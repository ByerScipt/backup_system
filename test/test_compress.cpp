#include "core/internal.hpp"
#include "helpers.hpp"

namespace fs = std::filesystem;
using namespace backup;

namespace
{

// Stage files use little-endian fields like every other pipeline stage.
struct Lz77Stream
{
    std::vector<uint8_t> bytes;

    explicit Lz77Stream(uint64_t originalSize) : bytes{'L', 'Z', '7', '7'}
    {
        append(originalSize, 8);
    }

    void append(uint64_t value, int count)
    {
        for (int index = 0; index < count; ++index)
        {
            bytes.push_back(static_cast<uint8_t>(value >> (index * 8)));
        }
    }

    // flags: 0 = token stream, 1 = stored block.
    Lz77Stream& block(uint8_t flags, uint32_t rawLength,
                      const std::vector<uint8_t>& payload)
    {
        bytes.push_back(flags);
        append(rawLength, 4);
        append(payload.size(), 4);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        return *this;
    }
};

std::vector<uint8_t> decodeStream(const fs::path& workspace,
                                  const Lz77Stream& stream,
                                  uint64_t expectedSize)
{
    const fs::path compressed = workspace / "manual.lz77";
    writeBytes(compressed, stream.bytes);
    detail::lz77Decompress(compressed, workspace / "manual.out", expectedSize,
                           RestoreOptions{});
    return readBytes(workspace / "manual.out");
}

// An empty expectedMessage only asserts that the stream is refused.
void expectStreamFailure(const fs::path& workspace,
                         const std::vector<uint8_t>& stream,
                         uint64_t expectedSize,
                         const std::string& expectedMessage)
{
    const fs::path compressed = workspace / "malformed.lz77";
    writeBytes(compressed, stream);
    std::string message;
    bool failed = false;
    try
    {
        detail::lz77Decompress(compressed, workspace / "malformed.out",
                               expectedSize, RestoreOptions{});
    }
    catch (const std::exception& error)
    {
        failed = true;
        message = error.what();
    }
    check(failed, "malformed LZ77 stream was accepted: " + expectedMessage);
    if (!expectedMessage.empty())
    {
        check(message.find(expectedMessage) != std::string::npos,
              "unexpected LZ77 rejection: expected '" + expectedMessage +
                  "' but got '" + message + "'");
    }
}

void roundTripLz77(const fs::path& source, const fs::path& compressed,
                   const fs::path& restored, const std::string& name,
                   const std::vector<uint8_t>& data, bool expectShrink)
{
    writeBytes(source, data);
    BackupOptions options;
    options.compression = CompressionAlgorithm::Lz77;
    bool compressing = false;
    options.progress = [&](const ProgressEvent& event)
    { compressing |= event.stage == "compress-lz77"; };
    detail::lz77Compress(source, compressed, options);
    check(compressing == !data.empty(),
          name + ": compression progress events do not match the input");

    // The block framing allows at most nine bytes of overhead per block, and
    // the encoder stores a block whenever the token stream is not smaller.
    const uint64_t blocks = data.size() / (1024u * 1024u) + 1;
    const uint64_t size = fs::file_size(compressed);
    check(size <= data.size() + 12 + 9 * blocks,
          name + ": LZ77 output grew beyond its framing overhead");
    if (expectShrink)
    {
        check(size < data.size() / 2, name + ": LZ77 did not compress");
    }

    RestoreOptions restore;
    bool decompressing = false;
    restore.progress = [&](const ProgressEvent& event)
    { decompressing |= event.stage == "decompress-lz77"; };
    detail::lz77Decompress(compressed, restored, data.size(), restore);
    check(decompressing == !data.empty(),
          name + ": decompression progress events do not match the input");
    check(readBytes(restored) == data, name + ": LZ77 round trip changed data");
}

} // namespace

void testLz77RoundTrip()
{
    TempDirectory temp;
    const fs::path source = temp.path / "input.bin";
    const fs::path compressed = temp.path / "stage.lz77";
    const fs::path restored = temp.path / "output.bin";

    roundTripLz77(source, compressed, restored, "empty", {}, false);
    roundTripLz77(source, compressed, restored, "one byte", {'x'}, false);
    roundTripLz77(source, compressed, restored, "below minimum match",
                  {'x', 'y'}, false);

    std::vector<uint8_t> repetitive;
    const std::string pattern =
        "line 0000: the quick brown fox jumps over the lazy dog\n";
    const size_t target = 2 * 1024 * 1024 + 4096; // two full blocks plus tail
    while (repetitive.size() < target)
    {
        repetitive.insert(repetitive.end(), pattern.begin(), pattern.end());
    }
    roundTripLz77(source, compressed, restored, "repetitive multi-block",
                  repetitive, true);

    // Chained digests are high-entropy data: the encoder must fall back to
    // stored blocks instead of expanding the payload.
    std::vector<uint8_t> incompressible;
    std::string seed = "lz77-incompressible";
    while (incompressible.size() < 1024u * 1024u)
    {
        const auto digest = sha256(seed);
        incompressible.insert(incompressible.end(), digest.begin(),
                              digest.end());
        seed = std::string(reinterpret_cast<const char*>(digest.data()),
                           digest.size());
    }
    roundTripLz77(source, compressed, restored, "incompressible",
                  incompressible, false);

    // Cancellation is checked between blocks, so a flag that is already set
    // must abort before any output is written.
    std::atomic_bool cancel{true};
    BackupOptions cancelled;
    cancelled.compression = CompressionAlgorithm::Lz77;
    cancelled.cancel = &cancel;
    bool aborted = false;
    try
    {
        detail::lz77Compress(source, compressed, cancelled);
    }
    catch (const std::exception&)
    {
        aborted = true;
    }
    check(aborted, "cancelled LZ77 compression still completed");
}

void testLz77HandBuiltStreams()
{
    TempDirectory temp;

    // A literal run followed by an overlapping match: "ab" plus a distance of
    // two reproduces "abababab".
    check(
        decodeStream(temp.path,
                     Lz77Stream(10).block(0, 10, {0x25, 'a', 'b', 0x02, 0x00}),
                     10) == std::vector<uint8_t>({'a', 'b', 'a', 'b', 'a', 'b',
                                                  'a', 'b', 'a', 'b'}),
        "overlapping LZ77 match did not decode");

    // One literal plus a long match with extension bytes and a distance of
    // one: the whole block becomes repeated 'A'.
    std::vector<uint8_t> longMatch{0x1F, 'A'};
    longMatch.insert(longMatch.end(), 15, 255);
    longMatch.push_back(252); // 15 + 15 * 255 + 252 = 4092, plus the minimum
    longMatch.push_back(0x01);
    longMatch.push_back(0x00);
    const auto repeated = decodeStream(
        temp.path, Lz77Stream(4096).block(0, 4096, longMatch), 4096);
    check(repeated == std::vector<uint8_t>(4096, 'A'),
          "long LZ77 match did not decode");

    // Stored blocks carry the raw bytes of an incompressible block.
    check(decodeStream(temp.path,
                       Lz77Stream(5).block(1, 5, {'h', 'e', 'l', 'l', 'o'}),
                       5) == std::vector<uint8_t>({'h', 'e', 'l', 'l', 'o'}),
          "stored LZ77 block did not decode");
}

void testLz77RejectsMalformedStreams()
{
    TempDirectory temp;
    const auto& path = temp.path;

    expectStreamFailure(path, {'n', 'o', 't', 'l', 'z'}, 0,
                        "invalid LZ77 header");
    expectStreamFailure(path, {'L', 'Z', '7'}, 0, "");
    expectStreamFailure(path, Lz77Stream(5).bytes, 6,
                        "LZ77 declared output size");

    // Block header validation.
    expectStreamFailure(
        path, Lz77Stream(5).block(2, 5, {'h', 'e', 'l', 'l', 'o'}).bytes, 5,
        "invalid LZ77 block flags");
    expectStreamFailure(path, Lz77Stream(5).block(0, 0, {}).bytes, 5,
                        "invalid LZ77 block size");
    expectStreamFailure(path, Lz77Stream(1048577).block(0, 1048577, {}).bytes,
                        1048577, "invalid LZ77 block size");
    expectStreamFailure(
        path, Lz77Stream(3).block(1, 5, {'h', 'e', 'l', 'l', 'o'}).bytes, 3,
        "LZ77 block exceeds output size limit");
    expectStreamFailure(path,
                        Lz77Stream(4).block(0, 4, {0x30, 'a', 'b', 'c'}).bytes,
                        4, "invalid LZ77 block payload size");
    expectStreamFailure(path, Lz77Stream(5).block(1, 5, {'h', 'e'}).bytes, 5,
                        "invalid LZ77 block payload size");

    // Token stream validation.
    expectStreamFailure(path, Lz77Stream(5).block(0, 5, {}).bytes, 5,
                        "truncated LZ77 token stream");
    expectStreamFailure(path, Lz77Stream(4).block(0, 4, {0x30, 'a'}).bytes, 4,
                        "truncated LZ77 literal run");
    expectStreamFailure(
        path, Lz77Stream(8).block(0, 8, {0x50, 'a', 'b', 'c', 'd', 'e'}).bytes,
        8, "truncated LZ77 match distance");
    expectStreamFailure(path,
                        Lz77Stream(100).block(0, 100, {0x0F, 255, 0}).bytes,
                        100, "LZ77 length exceeds block size");
    expectStreamFailure(
        path,
        Lz77Stream(10)
            .block(0, 10, {0x53, 'a', 'b', 'c', 'd', 'e', 0x00, 0x00})
            .bytes,
        10, "invalid LZ77 match distance");
    expectStreamFailure(
        path,
        Lz77Stream(10)
            .block(0, 10, {0x53, 'a', 'b', 'c', 'd', 'e', 0x07, 0x00})
            .bytes,
        10, "invalid LZ77 match distance");
    expectStreamFailure(
        path,
        Lz77Stream(10)
            .block(0, 10, {0x53, 'a', 'b', 'c', 'd', 'e', 0x05, 0x00})
            .bytes,
        10, "LZ77 match exceeds block size");
    expectStreamFailure(
        path,
        Lz77Stream(10)
            .block(0, 10, {0x52, 'a', 'b', 'c', 'd', 'e', 0x05, 0x00, 0xAB})
            .bytes,
        10, "LZ77 block has trailing data");

    // A completed stream must end exactly at its last block.
    Lz77Stream trailing(5);
    trailing.block(1, 5, {'h', 'e', 'l', 'l', 'o'});
    trailing.block(1, 5, {'w', 'o', 'r', 'l', 'd'});
    expectStreamFailure(path, trailing.bytes, 5,
                        "LZ77 stream has trailing data");
}

void testLz77ArchiveVersion()
{
    TempDirectory temp;
    const fs::path source = temp.path / "source";
    fs::create_directory(source);
    std::vector<uint8_t> payload(20000, 'r');
    payload.push_back('x');
    writeBytes(source / "payload.txt", payload);

    const fs::path archive = temp.path / "lz77.bak";
    BackupOptions options;
    options.compression = CompressionAlgorithm::Lz77;
    auto created =
        BackupEngine::create(source.string(), archive.string(), options);
    check(created.success, "LZ77 archive creation failed: " + created.message);
    const auto info = BackupEngine::inspect(archive.string());
    check(info.compression == CompressionAlgorithm::Lz77 && info.version == 3,
          "LZ77 archives must be written with version 3");
    check(fs::file_size(archive) < 20001,
          "LZ77 archive did not shrink the repeated payload");

    // Archives that do not use LZ77 stay on version 2, which older readers
    // still accept.
    const fs::path huffman = temp.path / "huffman.bak";
    options.compression = CompressionAlgorithm::Huffman;
    check(BackupEngine::create(source.string(), huffman.string(), options)
              .success,
          "Huffman archive creation failed");
    check(BackupEngine::inspect(huffman.string()).version == 2,
          "non-LZ77 archives must stay on version 2");

    const fs::path restored = temp.path / "restored";
    check(
        BackupEngine::restore(archive.string(), restored.string(), {}).success,
        "LZ77 archive did not restore");

    // A version 2 header claiming LZ77 must be refused rather than decoded
    // with the wrong expectations.
    std::vector<uint8_t> forged = readBytes(archive);
    forged[4] = 2;
    forged[5] = 0;
    const fs::path downgraded = temp.path / "downgraded.bak";
    writeBytes(downgraded, forged);
    auto rejected = BackupEngine::restore(
        downgraded.string(), (temp.path / "downgraded").string(), {});
    check(!rejected.success &&
              rejected.message.find("requires archive version 3") !=
                  std::string::npos,
          "version 2 archive claiming LZ77 was not rejected: " +
              rejected.message);
}
