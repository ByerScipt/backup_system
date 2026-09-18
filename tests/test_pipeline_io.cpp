#include "core/internal.hpp"
#include "helpers.hpp"

// White-box I/O seams: /dev/full reports ENOSPC without filling a real volume.
// Small writes exercise the final stream flush, not just writeExact().
void testPipelineIo()
{
    namespace pipeline = backup::detail;
    TempDirectory temp;
    const auto input = temp.path / "input";
    writeBytes(input, {'s', 'm', 'a', 'l', 'l'});
    const auto source = temp.path / "source";
    fs::create_directory(source);
    uint64_t size = 0;
    const auto entries = pipeline::scanDirectory(source, size, {});
    const auto rle = temp.path / "rle";
    const auto huffman = temp.path / "huffman";
    pipeline::rleCompress(input, rle, {});
    pipeline::huffmanCompress(input, huffman, {});
    const std::vector<std::pair<std::string, std::function<void()>>> operations{
        {"stream pack",
         [&] { pipeline::packStream(entries, "/dev/full", size, {}); }},
        {"index pack",
         [&] { pipeline::packIndex(entries, "/dev/full", size, {}); }},
        {"copy", [&]
         { pipeline::copyFileStage(input, "/dev/full", "copy", {}, nullptr); }},
        {"RLE encode", [&] { pipeline::rleCompress(input, "/dev/full", {}); }},
        {"RLE decode",
         [&] { pipeline::rleDecompress(rle, "/dev/full", 5, {}); }},
        {"Huffman encode",
         [&] { pipeline::huffmanCompress(input, "/dev/full", {}); }},
        {"Huffman decode",
         [&] { pipeline::huffmanDecompress(huffman, "/dev/full", 5, {}); }},
        {"ChaCha20",
         [&]
         {
             pipeline::cryptStage(input, "/dev/full",
                                  EncryptionAlgorithm::ChaCha20, "key", {}, {},
                                  nullptr, "encrypt");
         }},
        {"AES",
         [&]
         {
             pipeline::cryptStage(input, "/dev/full",
                                  EncryptionAlgorithm::Aes256, "key", {}, {},
                                  nullptr, "encrypt");
         }},
        {"no encryption", [&]
         {
             pipeline::cryptStage(input, "/dev/full", EncryptionAlgorithm::None,
                                  {}, {}, {}, nullptr, "copy");
         }}};
    for (const auto& operation : operations)
    {
        bool rejected = false;
        try
        {
            operation.second();
        }
        catch (const std::exception&)
        {
            rejected = true;
        }
        check(rejected,
              operation.first + " silently ignored a full output device");
    }
}
