#include "internal.hpp"

namespace backup {
namespace detail {

void ensure(bool condition, const std::string& message) {
    if (!condition)
        throw BackupError(message);
}

void checkCancelled(std::atomic_bool* cancel) {
    if (cancel && cancel->load())
        throw BackupError("operation cancelled");
}

void report(const ProgressCallback& callback, const std::string& stage, uint64_t completed,
            uint64_t total, const std::string& detail) {
    if (callback)
        callback({stage, completed, total, detail});
}

void writeExact(std::ostream& out, const void* data, size_t size) {
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    ensure(out.good(), "failed to write output stream");
}

void readExact(std::istream& in, void* data, size_t size) {
    in.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    ensure(in.good(), "truncated or unreadable input stream");
}

void writeU8(std::ostream& out, uint8_t value) {
    writeExact(out, &value, 1);
}

void writeU16(std::ostream& out, uint16_t value) {
    uint8_t b[2]{static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
    writeExact(out, b, sizeof(b));
}

void writeU32(std::ostream& out, uint32_t value) {
    uint8_t b[4];
    for (int i = 0; i < 4; ++i)
        b[i] = static_cast<uint8_t>(value >> (i * 8));
    writeExact(out, b, sizeof(b));
}

void writeU64(std::ostream& out, uint64_t value) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i)
        b[i] = static_cast<uint8_t>(value >> (i * 8));
    writeExact(out, b, sizeof(b));
}

uint8_t readU8(std::istream& in) {
    uint8_t v = 0;
    readExact(in, &v, 1);
    return v;
}

uint16_t readU16(std::istream& in) {
    uint8_t b[2];
    readExact(in, b, 2);
    return static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
}

uint32_t readU32(std::istream& in) {
    uint8_t b[4];
    readExact(in, b, 4);
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<uint32_t>(b[i]) << (i * 8);
    return v;
}

uint64_t readU64(std::istream& in) {
    uint8_t b[8];
    readExact(in, b, 8);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(b[i]) << (i * 8);
    return v;
}

void writeString(std::ostream& out, const std::string& value) {
    ensure(value.size() <= kMaxString, "archive string is too long");
    writeU32(out, static_cast<uint32_t>(value.size()));
    if (!value.empty())
        writeExact(out, value.data(), value.size());
}

std::string readString(std::istream& in) {
    uint32_t size = readU32(in);
    ensure(size <= kMaxString, "archive string length exceeds safety limit");
    std::string value(size, '\0');
    if (size)
        readExact(in, value.data(), size);
    ensure(value.find('\0') == std::string::npos, "archive path contains NUL");
    return value;
}

void copyBytes(std::istream& in, std::ostream& out, uint64_t count,
               const ProgressCallback& callback, const std::string& stage,
               std::atomic_bool* cancel) {
    std::vector<char> buffer(kBufferSize);
    uint64_t done = 0;
    while (done < count) {
        checkCancelled(cancel);
        size_t wanted = static_cast<size_t>(std::min<uint64_t>(buffer.size(), count - done));
        in.read(buffer.data(), static_cast<std::streamsize>(wanted));
        ensure(static_cast<size_t>(in.gcount()) == wanted, "truncated input while copying data");
        writeExact(out, buffer.data(), wanted);
        done += wanted;
        if (!stage.empty())
            report(callback, stage, done, count);
    }
}

TempFile::TempFile(const fs::path& directory) {
    fs::create_directories(directory);
    std::string pattern = (directory / ".backup-tmp-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    int fd = mkstemp(writable.data());
    ensure(fd >= 0, "cannot create secure temporary file");
    fchmod(fd, 0600);
    ::close(fd);
    path_ = writable.data();
}

TempFile::~TempFile() {
    std::error_code ec;
    fs::remove(path_, ec);
}

void commitFileNoReplace(const fs::path& temporary, const fs::path& output) {
#if defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD, output.c_str(),
                  RENAME_NOREPLACE) == 0)
        return;
    if (errno != ENOSYS && errno != EINVAL)
        throw BackupError("cannot atomically commit output without replacement: " +
                          std::string(std::strerror(errno)));
#endif
    ensure(::link(temporary.c_str(), output.c_str()) == 0,
           "cannot commit output without replacement: " + std::string(std::strerror(errno)));
    static_cast<void>(::unlink(temporary.c_str()));
}

bool isPathInside(const fs::path& child, const fs::path& parent) {
    auto c = child.lexically_normal();
    auto p = parent.lexically_normal();
    auto ci = c.begin(), pi = p.begin();
    for (; pi != p.end(); ++pi, ++ci) {
        if (ci == c.end() || *ci != *pi)
            return false;
    }
    return true;
}

size_t pathDepth(const fs::path& path) {
    return static_cast<size_t>(std::distance(path.begin(), path.end()));
}

void warnMetadata(const std::string& operation, const fs::path& path) {
    int error = errno;
    std::cerr << "Warning: " << operation << " failed for " << path << ": " << std::strerror(error)
              << '\n';
}
} // namespace detail
} // namespace backup
