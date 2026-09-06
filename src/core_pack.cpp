#include "core_internal.hpp"

namespace backup {
namespace detail {

void packStream(const std::vector<Entry>& entries, const fs::path& output,
                uint64_t inputBytes, const BackupOptions& options) {
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    ensure(out.good(), "cannot create stream archive stage");
    writeExact(out, kStreamMagic.data(), kStreamMagic.size());
    writeU32(out, static_cast<uint32_t>(entries.size()));
    uint64_t completed = 0;
    for (const auto& entry : entries) {
        writeU32(out, 0x52544E45u); // ENTR in little endian
        writeEntryMetadata(out, entry, false);
        if (entry.type == EntryType::Regular) copySourceFile(entry, out, completed, inputBytes, options);
    }
}

void packIndex(std::vector<Entry> entries, const fs::path& output,
               uint64_t inputBytes, const BackupOptions& options) {
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    ensure(out.good(), "cannot create indexed archive stage");
    writeExact(out, kIndexMagic.data(), kIndexMagic.size());
    uint64_t completed = 0;
    for (auto& entry : entries) {
        entry.contentOffset = static_cast<uint64_t>(out.tellp());
        if (entry.type == EntryType::Regular) copySourceFile(entry, out, completed, inputBytes, options);
    }
    uint64_t centralOffset = static_cast<uint64_t>(out.tellp());
    writeExact(out, kCentralMagic.data(), kCentralMagic.size());
    writeU32(out, static_cast<uint32_t>(entries.size()));
    for (const auto& entry : entries) writeEntryMetadata(out, entry, true);
    uint64_t centralEnd = static_cast<uint64_t>(out.tellp());
    writeExact(out, kIndexEndMagic.data(), kIndexEndMagic.size());
    writeU64(out, centralOffset);
    writeU64(out, centralEnd - centralOffset);
    writeU32(out, static_cast<uint32_t>(entries.size()));
    writeU32(out, 0);
}

std::vector<Entry> readStreamEntries(std::ifstream& in, uint64_t size) {
    std::array<char, 8> magic{}; readExact(in, magic.data(), magic.size());
    ensure(magic == kStreamMagic, "invalid sequential pack magic");
    uint32_t count = readU32(in);
    ensure(count > 0 && count <= kMaxEntries, "invalid sequential pack entry count");
    std::vector<Entry> entries;
    entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        ensure(readU32(in) == 0x52544E45u, "invalid sequential entry marker");
        Entry e = readEntryMetadata(in, false);
        auto pos = in.tellg();
        ensure(pos >= 0, "invalid sequential pack offset");
        e.contentOffset = static_cast<uint64_t>(pos);
        ensure(e.size <= size - std::min<uint64_t>(size, e.contentOffset), "truncated sequential entry");
        in.seekg(static_cast<std::streamoff>(e.size), std::ios::cur);
        ensure(in.good(), "truncated sequential pack");
        entries.push_back(std::move(e));
    }
    ensure(static_cast<uint64_t>(in.tellg()) == size, "sequential pack has trailing data");
    return entries;
}

std::vector<Entry> readIndexEntries(std::ifstream& in, uint64_t size) {
    ensure(size >= 40, "indexed pack is too small");
    std::array<char, 8> magic{}; readExact(in, magic.data(), magic.size());
    ensure(magic == kIndexMagic, "invalid indexed pack magic");
    in.seekg(static_cast<std::streamoff>(size - 32));
    std::array<char, 8> endMagic{}; readExact(in, endMagic.data(), endMagic.size());
    ensure(endMagic == kIndexEndMagic, "invalid indexed pack trailer");
    uint64_t centralOffset = readU64(in);
    uint64_t centralSize = readU64(in);
    uint32_t trailerCount = readU32(in);
    ensure(readU32(in) == 0, "non-zero indexed trailer reserved field");
    ensure(centralOffset >= 8 && centralOffset <= size - 32 &&
           centralSize <= size - 32 - centralOffset, "invalid central directory bounds");
    ensure(centralOffset + centralSize == size - 32,
           "central directory size does not reach the indexed trailer");
    in.seekg(static_cast<std::streamoff>(centralOffset));
    std::array<char, 8> centralMagic{}; readExact(in, centralMagic.data(), centralMagic.size());
    ensure(centralMagic == kCentralMagic, "invalid central directory magic");
    uint32_t count = readU32(in);
    ensure(count == trailerCount && count > 0 && count <= kMaxEntries, "invalid central directory count");
    std::vector<Entry> entries;
    entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Entry entry = readEntryMetadata(in, true);
        if (entry.type == EntryType::Regular) {
            ensure(entry.contentOffset >= 8 && entry.contentOffset <= centralOffset &&
                   entry.size <= centralOffset - entry.contentOffset,
                   "indexed file data overlaps the central directory");
        }
        entries.push_back(std::move(entry));
    }
    ensure(static_cast<uint64_t>(in.tellg()) == centralOffset + centralSize,
           "central directory size does not match its entries");
    return entries;
}

std::vector<Entry> readPackedEntries(const fs::path& packed, PackAlgorithm algorithm) {
    std::ifstream in(packed, std::ios::binary);
    ensure(in.good(), "cannot open packed stage");
    uint64_t size = static_cast<uint64_t>(fs::file_size(packed));
    std::vector<Entry> entries = algorithm == PackAlgorithm::Stream
        ? readStreamEntries(in, size) : readIndexEntries(in, size);
    validateEntries(entries, size);
    return entries;
}
} // namespace detail
} // namespace backup
