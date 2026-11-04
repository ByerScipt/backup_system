#include "internal.hpp"
#include <optional>

namespace backup::detail
{

uint16_t archiveVersionFor(CompressionAlgorithm compression)
{
    return compression == CompressionAlgorithm::Lz77 ? kArchiveVersionLz77
                                                     : kArchiveVersion;
}

void writeArchiveHeader(std::ostream& out, const ArchiveInfo& info,
                        const std::array<uint8_t, 16>& salt)
{
    writeExact(out, kArchiveMagic.data(), kArchiveMagic.size());
    writeU16(out, info.version);
    writeU16(out, kArchiveHeaderSize);
    uint32_t flags = 1u;
    if (info.compression != CompressionAlgorithm::None)
    {
        flags |= 2u;
    }
    if (info.encryption != EncryptionAlgorithm::None)
    {
        flags |= 4u;
    }
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

ParsedHeader readArchiveHeader(std::istream& in)
{
    ParsedHeader parsed;
    std::array<char, 4> magic{};
    readExact(in, magic.data(), magic.size());
    ensure(magic == kArchiveMagic, "invalid archive magic; expected BKP2");
    parsed.info.version = readU16(in);
    ensure(parsed.info.version == 1 || parsed.info.version == kArchiveVersion ||
               parsed.info.version == kArchiveVersionLz77 ||
               parsed.info.version == kArchiveVersionAuthenticated,
           "unsupported archive version");
    ensure(readU16(in) == kArchiveHeaderSize,
           "unsupported archive header size");
    uint32_t flags = readU32(in);
    uint8_t pack = readU8(in), compression = readU8(in),
            encryption = readU8(in);
    ensure(readU8(in) == 0, "non-zero archive reserved byte");
    ensure(pack == 1 || pack == 2, "invalid pack algorithm identifier");
    ensure(compression <= 3 &&
               (encryption == 0 || encryption == 3 || encryption == 4),
           "invalid transform algorithm identifier");
    ensure(parsed.info.version >= kArchiveVersionLz77 || compression != 3,
           "LZ77 compression requires archive version 3");
    ensure(encryption != 1 && encryption != 2,
           "legacy XOR/Vigenere archives are no longer supported");
    parsed.info.pack = static_cast<PackAlgorithm>(pack);
    parsed.info.compression = static_cast<CompressionAlgorithm>(compression);
    parsed.info.encryption = static_cast<EncryptionAlgorithm>(encryption);
    ensure(parsed.info.version != kArchiveVersionAuthenticated ||
               parsed.info.encryption != EncryptionAlgorithm::None,
           "authenticated archives require encryption");
    ensure((flags & 1u) && ((flags & 2u) != 0) == (compression != 0) &&
               ((flags & 4u) != 0) == (encryption != 0) && (flags & ~7u) == 0,
           "archive flags do not match algorithm identifiers");
    parsed.info.packedSize = readU64(in);
    parsed.info.encodedSize = readU64(in);
    readExact(in, parsed.salt.data(), parsed.salt.size());
    readExact(in, parsed.info.packedDigest.data(),
              parsed.info.packedDigest.size());
    readExact(in, parsed.info.encodedDigest.data(),
              parsed.info.encodedDigest.size());
    return parsed;
}

ParsedHeader readArchiveHeader(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    ensure(in.good(), "cannot open archive: " + path.string());
    ParsedHeader parsed = readArchiveHeader(in);
    uint64_t size = fileSizeChecked(path);
    const auto overhead =
        kArchiveHeaderSize +
        (parsed.info.version == kArchiveVersionAuthenticated ? kArchiveTagSize
                                                             : 0);
    ensure(size >= static_cast<uint64_t>(overhead) &&
               parsed.info.encodedSize == size - overhead,
           "archive payload size does not match header");
    return parsed;
}

std::array<uint8_t, 32> archiveAuthentication(
    const fs::path& archive, uint64_t size, const std::string& password,
    const std::array<uint8_t, 16>& salt, std::atomic_bool* cancel)
{
    // Domain separation keeps the authentication key distinct from the
    // existing encryption key. The MAC covers the complete header and cipher.
    const auto encryptionKey = deriveKey(password, salt);
    const std::string label = "BKP2-v4-authentication";
    const auto key = hmacSha256({encryptionKey.begin(), encryptionKey.end()},
                                {label.begin(), label.end()});
    return hmacFile(archive, key, size, cancel);
}

void withDecodedArchive(const fs::path& archive, const RestoreOptions& options,
                        const DecodedArchiveConsumer& consumer,
                        const fs::path& workDirectory)
{
    checkCancelled(options.cancel);
    ParsedHeader header = readArchiveHeader(archive);
    const bool encrypted = header.info.encryption != EncryptionAlgorithm::None;
    const bool authenticated =
        header.info.version == kArchiveVersionAuthenticated;
    if (encrypted)
    {
        ensure(!options.password.empty(),
               "archive is encrypted; a password is required");
        ensure(authenticated || options.allowLegacyEncryption,
               "legacy encrypted archive has no authentication; explicit "
               "opt-in required");
    }
    // Keep a private snapshot so validation and extraction use the same bytes,
    // even if another process replaces or modifies the original archive.
    auto stage = std::make_unique<TempFile>(workDirectory);
    {
        std::ifstream in(archive, std::ios::binary);
        std::ofstream out(stage->path(), std::ios::binary | std::ios::trunc);
        ensure(in.good() && out.good(), "cannot prepare restore payload");
        if (encrypted)
        {
            writeArchiveHeader(out, header.info, header.salt);
        }
        in.seekg(kArchiveHeaderSize);
        copyBytes(in, out,
                  header.info.encodedSize +
                      (authenticated ? kArchiveTagSize : 0),
                  options.progress, "read-archive", options.cancel);
        closeOutput(out);
    }
    fs::path packedPath = stage->path();
    const uint64_t offset = encrypted ? kArchiveHeaderSize : 0;
    if (authenticated)
    {
        const auto size = kArchiveHeaderSize + header.info.encodedSize;
        const auto actual = archiveAuthentication(
            packedPath, size, options.password, header.salt, options.cancel);
        std::ifstream in(packedPath, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(size));
        std::array<uint8_t, 32> expected{};
        readExact(in, expected.data(), expected.size());
        uint8_t different = 0;
        for (size_t index = 0; index < actual.size(); ++index)
        {
            different |= actual[index] ^ expected[index];
        }
        ensure(different == 0, "archive authentication failed (wrong password "
                               "or tampered archive)");
        report(options.progress, "authenticate", size, size, "HMAC-SHA-256");
    }
    ensure(shaFileRange(packedPath, offset, header.info.encodedSize,
                        options.cancel) == header.info.encodedDigest,
           "archive payload checksum mismatch");
    if (encrypted)
    {
        auto decoded = std::make_unique<TempFile>(workDirectory);
        cryptStage(packedPath, decoded->path(), header.info.encryption,
                   options.password, header.salt, options.progress,
                   options.cancel, "decrypt", offset, header.info.encodedSize);
        stage = std::move(decoded);
        packedPath = stage->path();
    }
    else
    {
        report(options.progress, "decrypt", header.info.encodedSize,
               header.info.encodedSize);
    }
    checkCancelled(options.cancel);
    if (header.info.compression != CompressionAlgorithm::None)
    {
        auto decoded = std::make_unique<TempFile>(workDirectory);
        decompressStage(packedPath, decoded->path(), header.info.compression,
                        header.info.packedSize, options);
        stage = std::move(decoded);
        packedPath = stage->path();
    }
    else
    {
        report(options.progress, "decompress-copy", header.info.packedSize,
               header.info.packedSize);
    }
    checkCancelled(options.cancel);
    ensure(fileSizeChecked(packedPath) == header.info.packedSize,
           "decoded packed size mismatch (wrong password or damaged archive)");
    const bool unchanged =
        !encrypted && header.info.compression == CompressionAlgorithm::None;
    ensure((unchanged
                ? header.info.encodedDigest
                : shaFileRange(packedPath, 0, UINT64_MAX, options.cancel)) ==
               header.info.packedDigest,
           "decoded checksum mismatch (wrong password or damaged archive)");

    auto entries = readPackedEntries(packedPath, header.info.pack);
    if (header.info.version == 1)
    {
        ensure(std::none_of(entries.begin(), entries.end(),
                            [](const Entry& entry)
                            { return entry.type == EntryType::Hardlink; }),
               "hardlinks require archive version 2");
    }
    checkCancelled(options.cancel);
    consumer(header, packedPath, entries);
}

fs::path restoreWorkDirectory(const fs::path& destination)
{
    auto candidate = fs::absolute(destination);
    while (!fs::exists(candidate))
    {
        candidate = candidate.parent_path();
    }
    ensure(fs::is_directory(candidate) && !fs::is_symlink(candidate),
           "restore destination must be a real directory");
    return candidate;
}

std::string entryTypeName(EntryType type)
{
    switch (type)
    {
    case EntryType::Regular:
        return "file";
    case EntryType::Directory:
        return "directory";
    case EntryType::Symlink:
        return "symlink";
    case EntryType::Fifo:
        return "fifo";
    case EntryType::Character:
        return "character-device";
    case EntryType::Block:
        return "block-device";
    case EntryType::Socket:
        return "unix-socket";
    case EntryType::Hardlink:
        return "hardlink";
    }
    return "unknown";
}
} // namespace backup::detail
