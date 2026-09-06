#include "core_internal.hpp"

namespace backup {

BackupResult BackupEngine::create(const std::string& sourceDirectory,
                                  const std::string& archivePath,
                                  const BackupOptions& options) {
    BackupResult result;
    try {
        if (options.encryption != EncryptionAlgorithm::None) {
            detail::ensure(!options.password.empty(), "an encryption password is required");
        }
        fs::path source = fs::canonical(sourceDirectory);
        fs::path output = fs::absolute(archivePath).lexically_normal();
        fs::path outputParent = output.parent_path().empty() ? fs::current_path() : output.parent_path();
        std::error_code ec;
        fs::create_directories(outputParent, ec);
        detail::ensure(!ec, "cannot create archive output directory");
        fs::path canonicalParent = fs::canonical(outputParent, ec);
        detail::ensure(!ec, "cannot canonicalize archive output directory");
        fs::path canonicalOutput = canonicalParent / output.filename();
        detail::ensure(!detail::isPathInside(canonicalOutput, source), "archive output cannot be inside the source directory");
        struct stat outputState{};
        detail::ensure(lstat(canonicalOutput.c_str(), &outputState) != 0,
               "archive output already exists");
        detail::ensure(errno == ENOENT, "cannot inspect archive output path");

        uint64_t inputBytes = 0;
        auto entries = detail::scanDirectory(source, inputBytes, options);
        detail::TempFile packed;
        if (options.pack == PackAlgorithm::Stream) detail::packStream(entries, packed.path(), inputBytes, options);
        else if (options.pack == PackAlgorithm::Index) detail::packIndex(entries, packed.path(), inputBytes, options);
        else throw detail::BackupError("unsupported pack algorithm");

        ArchiveInfo info;
        info.version = detail::kArchiveVersion;
        info.pack = options.pack;
        info.compression = options.compression;
        info.encryption = options.encryption;
        info.packedSize = detail::fileSizeChecked(packed.path());
        info.packedDigest = detail::shaFileRange(packed.path(), 0, UINT64_MAX);

        detail::TempFile compressed;
        detail::compressStage(packed.path(), compressed.path(), options.compression, options);
        auto salt = detail::randomSalt();
        detail::TempFile encoded;
        detail::cryptStage(compressed.path(), encoded.path(), options.encryption, options.password,
                   salt, options.progress, options.cancel, "encrypt");
        info.encodedSize = detail::fileSizeChecked(encoded.path());
        info.encodedDigest = detail::shaFileRange(encoded.path(), 0, UINT64_MAX);

        detail::TempFile finalTemp(canonicalParent);
        std::ofstream out(finalTemp.path(), std::ios::binary | std::ios::trunc);
        detail::ensure(out.good(), "cannot create final archive");
        detail::writeArchiveHeader(out, info, salt);
        std::ifstream payload(encoded.path(), std::ios::binary);
        detail::ensure(payload.good(), "cannot reopen encoded payload");
        detail::copyBytes(payload, out, info.encodedSize, options.progress, "finalize", options.cancel);
        out.close();
        detail::ensure(out.good(), "cannot close final archive");
        int fd = ::open(finalTemp.path().c_str(), O_RDONLY);
        detail::ensure(fd >= 0, "cannot open final archive for synchronization");
        detail::ensure(::fsync(fd) == 0, "cannot synchronize final archive");
        detail::ensure(::close(fd) == 0, "cannot close synchronized final archive");
        detail::commitFileNoReplace(finalTemp.path(), canonicalOutput);
        int parentFd = ::open(canonicalParent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        detail::ensure(parentFd >= 0, "cannot open archive parent directory for synchronization");
        detail::ensure(::fsync(parentFd) == 0, "cannot synchronize archive parent directory");
        detail::ensure(::close(parentFd) == 0, "cannot close archive parent directory");

        result.success = true;
        result.message = "backup completed";
        result.entryCount = entries.size();
        result.inputBytes = inputBytes;
        result.outputBytes = detail::kArchiveHeaderSize + info.encodedSize;
    } catch (const std::exception& error) {
        result.message = error.what();
    }
    return result;
}

BackupResult BackupEngine::restore(const std::string& archivePath,
                                   const std::string& destinationDirectory,
                                   const RestoreOptions& options) {
    BackupResult result;
    try {
        fs::path archive = fs::canonical(archivePath);
        uint64_t outputBytes = 0;
        detail::withDecodedArchive(archive, options, [&](const detail::ParsedHeader&, const fs::path& packed,
                                                 const std::vector<detail::Entry>& entries) {
            detail::extractEntries(packed, fs::absolute(destinationDirectory), entries, options, outputBytes);
            result.entryCount = entries.size();
        });
        result.success = true;
        result.message = "restore completed";
        result.inputBytes = detail::fileSizeChecked(archive);
        result.outputBytes = outputBytes;
    } catch (const std::exception& error) {
        result.message = error.what();
    }
    return result;
}

RestorePreview BackupEngine::preview(const std::string& archivePath,
                                     const std::string& destinationDirectory,
                                     const std::string& password) {
    fs::path archive = fs::canonical(archivePath);
    RestoreOptions options;
    options.password = password;
    RestorePreview preview;
    detail::withDecodedArchive(archive, options, [&](const detail::ParsedHeader&, const fs::path&,
                                             const std::vector<detail::Entry>& entries) {
        fs::path destination = fs::absolute(destinationDirectory).lexically_normal();
        std::error_code ec;
        if (fs::exists(destination, ec)) {
            detail::ensure(!ec && fs::is_directory(destination) && !fs::is_symlink(destination),
                   "restore destination must be a real directory");
            destination = fs::canonical(destination, ec);
            detail::ensure(!ec, "cannot canonicalize restore destination");
        } else {
            detail::ensure(!ec, "cannot inspect restore destination");
        }
        preview.entries.reserve(entries.size());
        for (const auto& entry : entries) {
            preview.entries.push_back({entry.path, detail::entryTypeName(entry.type), entry.size});
            struct stat st{};
            fs::path target = destination / fs::path(entry.path);
            if (lstat(target.c_str(), &st) == 0) preview.conflicts.push_back(entry.path);
            else detail::ensure(errno == ENOENT, "cannot inspect restore target: " + target.string());
        }
    });
    return preview;
}

ArchiveInfo BackupEngine::inspect(const std::string& archivePath) {
    fs::path archive = fs::canonical(archivePath);
    detail::ParsedHeader header = detail::readArchiveHeader(archive);
    detail::ensure(detail::shaFileRange(archive, detail::kArchiveHeaderSize, header.info.encodedSize) == header.info.encodedDigest,
           "archive payload checksum mismatch");
    return header.info;
}

std::string toString(PackAlgorithm value) {
    switch (value) { case PackAlgorithm::Stream: return "stream"; case PackAlgorithm::Index: return "index"; }
    return "unknown";
}

std::string toString(CompressionAlgorithm value) {
    switch (value) {
    case CompressionAlgorithm::None: return "none";
    case CompressionAlgorithm::Rle: return "rle";
    case CompressionAlgorithm::Huffman: return "huffman";
    }
    return "unknown";
}

std::string toString(EncryptionAlgorithm value) {
    switch (value) {
    case EncryptionAlgorithm::None: return "none";
    case EncryptionAlgorithm::Xor: return "xor";
    case EncryptionAlgorithm::Vigenere: return "vigenere";
    }
    return "unknown";
}

PackAlgorithm parsePackAlgorithm(const std::string& value) {
    if (value == "stream") return PackAlgorithm::Stream;
    if (value == "index") return PackAlgorithm::Index;
    throw std::invalid_argument("unknown pack algorithm: " + value);
}

CompressionAlgorithm parseCompressionAlgorithm(const std::string& value) {
    if (value == "none") return CompressionAlgorithm::None;
    if (value == "rle") return CompressionAlgorithm::Rle;
    if (value == "huffman") return CompressionAlgorithm::Huffman;
    throw std::invalid_argument("unknown compression algorithm: " + value);
}

EncryptionAlgorithm parseEncryptionAlgorithm(const std::string& value) {
    if (value == "none") return EncryptionAlgorithm::None;
    if (value == "xor") return EncryptionAlgorithm::Xor;
    if (value == "vigenere") return EncryptionAlgorithm::Vigenere;
    throw std::invalid_argument("unknown encryption algorithm: " + value);
}

} // namespace backup
