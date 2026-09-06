#include "core_internal.hpp"

namespace backup {
namespace detail {

void writeArchiveHeader(std::ostream& out, const ArchiveInfo& info,
                        const std::array<uint8_t,16>& salt) {
    writeExact(out, kArchiveMagic.data(), kArchiveMagic.size());
    writeU16(out, kArchiveVersion);
    writeU16(out, kArchiveHeaderSize);
    uint32_t flags = 1u;
    if (info.compression != CompressionAlgorithm::None) flags |= 2u;
    if (info.encryption != EncryptionAlgorithm::None) flags |= 4u;
    writeU32(out, flags);
    writeU8(out, static_cast<uint8_t>(info.pack));
    writeU8(out, static_cast<uint8_t>(info.compression));
    writeU8(out, static_cast<uint8_t>(info.encryption));
    writeU8(out, 0);
    writeU64(out, info.packedSize);
    writeU64(out, info.encodedSize);
    writeExact(out, salt.data(), salt.size());
    writeExact(out, info.packedDigest.data(), info.packedDigest.size());
    writeExact(out, info.encodedDigest.data(), info.encodedDigest.size());
}

ParsedHeader readArchiveHeader(std::istream& in) {
    ParsedHeader parsed;
    std::array<char,4> magic{}; readExact(in, magic.data(), magic.size());
    ensure(magic == kArchiveMagic, "invalid archive magic; expected BKP2");
    parsed.info.version = readU16(in);
    ensure(parsed.info.version == kArchiveVersion, "unsupported archive version");
    ensure(readU16(in) == kArchiveHeaderSize, "unsupported archive header size");
    uint32_t flags = readU32(in);
    uint8_t pack = readU8(in), compression = readU8(in), encryption = readU8(in);
    ensure(readU8(in) == 0, "non-zero archive reserved byte");
    ensure(pack == 1 || pack == 2, "invalid pack algorithm identifier");
    ensure(compression <= 2 && encryption <= 2, "invalid transform algorithm identifier");
    parsed.info.pack = static_cast<PackAlgorithm>(pack);
    parsed.info.compression = static_cast<CompressionAlgorithm>(compression);
    parsed.info.encryption = static_cast<EncryptionAlgorithm>(encryption);
    ensure((flags & 1u) && ((flags & 2u) != 0) == (compression != 0) &&
           ((flags & 4u) != 0) == (encryption != 0) && (flags & ~7u) == 0,
           "archive flags do not match algorithm identifiers");
    parsed.info.packedSize = readU64(in);
    parsed.info.encodedSize = readU64(in);
    readExact(in, parsed.salt.data(), parsed.salt.size());
    readExact(in, parsed.info.packedDigest.data(), parsed.info.packedDigest.size());
    readExact(in, parsed.info.encodedDigest.data(), parsed.info.encodedDigest.size());
    return parsed;
}

ParsedHeader readArchiveHeader(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    ensure(in.good(), "cannot open archive: " + path.string());
    ParsedHeader parsed = readArchiveHeader(in);
    uint64_t size = fileSizeChecked(path);
    ensure(size >= kArchiveHeaderSize && parsed.info.encodedSize == size - kArchiveHeaderSize,
           "archive payload size does not match header");
    return parsed;
}

void withDecodedArchive(const fs::path& archive, const RestoreOptions& options,
                        const DecodedArchiveConsumer& consumer) {
    ParsedHeader header = readArchiveHeader(archive);
    if (header.info.encryption != EncryptionAlgorithm::None)
        ensure(!options.password.empty(), "archive is encrypted; a password is required");
    auto encodedDigest = shaFileRange(archive, kArchiveHeaderSize, header.info.encodedSize);
    ensure(encodedDigest == header.info.encodedDigest, "archive payload checksum mismatch");

    TempFile encoded;
    {
        std::ifstream in(archive, std::ios::binary);
        std::ofstream out(encoded.path(), std::ios::binary | std::ios::trunc);
        ensure(in.good() && out.good(), "cannot prepare restore payload");
        in.seekg(kArchiveHeaderSize);
        copyBytes(in, out, header.info.encodedSize, options.progress, "read-archive", options.cancel);
    }
    TempFile compressed;
    cryptStage(encoded.path(), compressed.path(), header.info.encryption, options.password,
               header.salt, options.progress, options.cancel, "decrypt");
    TempFile packed;
    decompressStage(compressed.path(), packed.path(), header.info.compression,
                    header.info.packedSize, options);
    ensure(fileSizeChecked(packed.path()) == header.info.packedSize,
           "decoded packed size mismatch (wrong password or damaged archive)");
    ensure(shaFileRange(packed.path(), 0, UINT64_MAX) == header.info.packedDigest,
           "decoded checksum mismatch (wrong password or damaged archive)");

    auto entries = readPackedEntries(packed.path(), header.info.pack);
    consumer(header, packed.path(), entries);
}

std::string entryTypeName(EntryType type) {
    switch (type) {
    case EntryType::Regular: return "file";
    case EntryType::Directory: return "directory";
    case EntryType::Symlink: return "symlink";
    case EntryType::Fifo: return "fifo";
    case EntryType::Character: return "character-device";
    case EntryType::Block: return "block-device";
    case EntryType::Socket: return "unix-socket";
    }
    return "unknown";
}
} // namespace detail
} // namespace backup
