#include "backup/core.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <fcntl.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace backup {
namespace {

constexpr size_t kBufferSize = 1024 * 1024;
constexpr uint64_t kMaxEntries = 1'000'000;
constexpr uint32_t kMaxString = 1024 * 1024;
constexpr uint16_t kArchiveVersion = 1;
constexpr uint16_t kArchiveHeaderSize = 112;
constexpr std::array<char, 4> kArchiveMagic{{'B', 'K', 'P', '2'}};
constexpr std::array<char, 8> kStreamMagic{{'S', 'T', 'R', 'M', 'P', 'K', '1', '\0'}};
constexpr std::array<char, 8> kIndexMagic{{'I', 'N', 'D', 'X', 'P', 'K', '1', '\0'}};
constexpr std::array<char, 8> kCentralMagic{{'C', 'D', 'I', 'R', 'V', '1', '\0', '\0'}};
constexpr std::array<char, 8> kIndexEndMagic{{'I', 'D', 'X', 'E', 'N', 'D', '1', '\0'}};

class BackupError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void ensure(bool condition, const std::string& message) {
    if (!condition) throw BackupError(message);
}

void checkCancelled(std::atomic_bool* cancel) {
    if (cancel && cancel->load()) throw BackupError("operation cancelled");
}

void report(const ProgressCallback& callback, const std::string& stage,
            uint64_t completed, uint64_t total, const std::string& detail = {}) {
    if (callback) callback({stage, completed, total, detail});
}

void writeExact(std::ostream& out, const void* data, size_t size) {
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    ensure(out.good(), "failed to write output stream");
}

void readExact(std::istream& in, void* data, size_t size) {
    in.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    ensure(in.good(), "truncated or unreadable input stream");
}

void writeU8(std::ostream& out, uint8_t value) { writeExact(out, &value, 1); }

void writeU16(std::ostream& out, uint16_t value) {
    uint8_t b[2]{static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
    writeExact(out, b, sizeof(b));
}

void writeU32(std::ostream& out, uint32_t value) {
    uint8_t b[4];
    for (int i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>(value >> (i * 8));
    writeExact(out, b, sizeof(b));
}

void writeU64(std::ostream& out, uint64_t value) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(value >> (i * 8));
    writeExact(out, b, sizeof(b));
}

uint8_t readU8(std::istream& in) {
    uint8_t v = 0;
    readExact(in, &v, 1);
    return v;
}

uint16_t readU16(std::istream& in) {
    uint8_t b[2]; readExact(in, b, 2);
    return static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
}

uint32_t readU32(std::istream& in) {
    uint8_t b[4]; readExact(in, b, 4);
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(b[i]) << (i * 8);
    return v;
}

uint64_t readU64(std::istream& in) {
    uint8_t b[8]; readExact(in, b, 8);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[i]) << (i * 8);
    return v;
}

void writeString(std::ostream& out, const std::string& value) {
    ensure(value.size() <= kMaxString, "archive string is too long");
    writeU32(out, static_cast<uint32_t>(value.size()));
    if (!value.empty()) writeExact(out, value.data(), value.size());
}

std::string readString(std::istream& in) {
    uint32_t size = readU32(in);
    ensure(size <= kMaxString, "archive string length exceeds safety limit");
    std::string value(size, '\0');
    if (size) readExact(in, value.data(), size);
    ensure(value.find('\0') == std::string::npos, "archive path contains NUL");
    return value;
}

void copyBytes(std::istream& in, std::ostream& out, uint64_t count,
               const ProgressCallback& callback = {}, const std::string& stage = {},
               std::atomic_bool* cancel = nullptr) {
    std::vector<char> buffer(kBufferSize);
    uint64_t done = 0;
    while (done < count) {
        checkCancelled(cancel);
        size_t wanted = static_cast<size_t>(std::min<uint64_t>(buffer.size(), count - done));
        in.read(buffer.data(), static_cast<std::streamsize>(wanted));
        ensure(static_cast<size_t>(in.gcount()) == wanted, "truncated input while copying data");
        writeExact(out, buffer.data(), wanted);
        done += wanted;
        if (!stage.empty()) report(callback, stage, done, count);
    }
}

class Sha256 {
public:
    Sha256() { reset(); }

    void update(const uint8_t* data, size_t len) {
        total_ += len;
        while (len > 0) {
            size_t take = std::min(len, block_.size() - used_);
            std::memcpy(block_.data() + used_, data, take);
            used_ += take;
            data += take;
            len -= take;
            if (used_ == block_.size()) {
                transform(block_.data());
                used_ = 0;
            }
        }
    }

    std::array<uint8_t, 32> finish() {
        uint64_t bitLength = total_ * 8;
        block_[used_++] = 0x80;
        if (used_ > 56) {
            std::fill(block_.begin() + static_cast<ptrdiff_t>(used_), block_.end(), 0);
            transform(block_.data());
            used_ = 0;
        }
        std::fill(block_.begin() + static_cast<ptrdiff_t>(used_), block_.begin() + 56, 0);
        for (int i = 0; i < 8; ++i) block_[63 - i] = static_cast<uint8_t>(bitLength >> (i * 8));
        transform(block_.data());

        std::array<uint8_t, 32> result{};
        for (size_t i = 0; i < state_.size(); ++i) {
            result[i * 4] = static_cast<uint8_t>(state_[i] >> 24);
            result[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
            result[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
            result[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
        }
        return result;
    }

private:
    std::array<uint32_t, 8> state_{};
    std::array<uint8_t, 64> block_{};
    size_t used_ = 0;
    uint64_t total_ = 0;

    static uint32_t rotr(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }
    static uint32_t choose(uint32_t e, uint32_t f, uint32_t g) { return (e & f) ^ (~e & g); }
    static uint32_t majority(uint32_t a, uint32_t b, uint32_t c) { return (a & b) ^ (a & c) ^ (b & c); }

    void reset() {
        state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                  0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        used_ = 0;
        total_ = 0;
    }

    void transform(const uint8_t* chunk) {
        static constexpr uint32_t k[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
        };
        uint32_t w[64]{};
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(chunk[i * 4]) << 24) |
                   (static_cast<uint32_t>(chunk[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(chunk[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(chunk[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=state_[0], b=state_[1], c=state_[2], d=state_[3];
        uint32_t e=state_[4], f=state_[5], g=state_[6], h=state_[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            uint32_t t1 = h + s1 + choose(e,f,g) + k[i] + w[i];
            uint32_t s0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            uint32_t t2 = s0 + majority(a,b,c);
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d;
        state_[4]+=e; state_[5]+=f; state_[6]+=g; state_[7]+=h;
    }
};

std::array<uint8_t, 32> shaFileRange(const fs::path& path, uint64_t offset, uint64_t length) {
    std::ifstream in(path, std::ios::binary);
    ensure(in.good(), "cannot open file for SHA-256: " + path.string());
    uint64_t total = static_cast<uint64_t>(fs::file_size(path));
    ensure(offset <= total, "SHA-256 offset exceeds file size");
    uint64_t available = total - offset;
    uint64_t count = length == UINT64_MAX ? available : std::min(length, available);
    ensure(length == UINT64_MAX || length <= available, "SHA-256 range exceeds file size");
    in.seekg(static_cast<std::streamoff>(offset));
    Sha256 sha;
    std::vector<uint8_t> buffer(kBufferSize);
    uint64_t done = 0;
    while (done < count) {
        size_t take = static_cast<size_t>(std::min<uint64_t>(buffer.size(), count - done));
        readExact(in, buffer.data(), take);
        sha.update(buffer.data(), take);
        done += take;
    }
    return sha.finish();
}

class TempFile {
public:
    explicit TempFile(const fs::path& directory = fs::temp_directory_path()) {
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
    ~TempFile() {
        if (!keep_) {
            std::error_code ec;
            fs::remove(path_, ec);
        }
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    const fs::path& path() const { return path_; }
    void keep() { keep_ = true; }
private:
    fs::path path_;
    bool keep_ = false;
};

void commitFileNoReplace(const fs::path& temporary, const fs::path& output) {
#if defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD,
                  output.c_str(), RENAME_NOREPLACE) == 0) return;
    if (errno != ENOSYS && errno != EINVAL)
        throw BackupError("cannot atomically commit output without replacement: " +
                          std::string(std::strerror(errno)));
#endif
    ensure(::link(temporary.c_str(), output.c_str()) == 0,
           "cannot commit output without replacement: " + std::string(std::strerror(errno)));
    static_cast<void>(::unlink(temporary.c_str()));
}

enum class EntryType : uint8_t {
    Regular = 1, Directory = 2, Symlink = 3, Fifo = 4,
    Character = 5, Block = 6, Socket = 7
};

struct Entry {
    std::string path;
    fs::path sourcePath;
    EntryType type = EntryType::Regular;
    uint32_t mode = 0;
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint64_t size = 0;
    int64_t atimeSec = 0;
    uint32_t atimeNsec = 0;
    int64_t mtimeSec = 0;
    uint32_t mtimeNsec = 0;
    int64_t ctimeSec = 0;
    uint32_t ctimeNsec = 0;
    std::string linkTarget;
    uint64_t device = 0;
    uint64_t contentOffset = 0;
    uint64_t sourceDevice = 0;
    uint64_t sourceInode = 0;
};

EntryType entryTypeFromMode(mode_t mode) {
    if (S_ISREG(mode)) return EntryType::Regular;
    if (S_ISDIR(mode)) return EntryType::Directory;
    if (S_ISLNK(mode)) return EntryType::Symlink;
    if (S_ISFIFO(mode)) return EntryType::Fifo;
    if (S_ISCHR(mode)) return EntryType::Character;
    if (S_ISBLK(mode)) return EntryType::Block;
    if (S_ISSOCK(mode)) return EntryType::Socket;
    throw BackupError("unsupported filesystem entry type");
}

Entry makeEntry(const fs::path& full, const std::string& archivePath) {
    struct stat st{};
    ensure(lstat(full.c_str(), &st) == 0, "cannot stat source entry: " + full.string());
    Entry e;
    e.path = archivePath;
    e.sourcePath = full;
    e.sourceDevice = static_cast<uint64_t>(st.st_dev);
    e.sourceInode = static_cast<uint64_t>(st.st_ino);
    e.type = entryTypeFromMode(st.st_mode);
    e.mode = static_cast<uint32_t>(st.st_mode & 07777);
    e.uid = static_cast<uint32_t>(st.st_uid);
    e.gid = static_cast<uint32_t>(st.st_gid);
    e.size = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
#ifdef __APPLE__
    e.atimeSec = st.st_atimespec.tv_sec; e.atimeNsec = static_cast<uint32_t>(st.st_atimespec.tv_nsec);
    e.mtimeSec = st.st_mtimespec.tv_sec; e.mtimeNsec = static_cast<uint32_t>(st.st_mtimespec.tv_nsec);
    e.ctimeSec = st.st_ctimespec.tv_sec; e.ctimeNsec = static_cast<uint32_t>(st.st_ctimespec.tv_nsec);
#else
    e.atimeSec = st.st_atim.tv_sec; e.atimeNsec = static_cast<uint32_t>(st.st_atim.tv_nsec);
    e.mtimeSec = st.st_mtim.tv_sec; e.mtimeNsec = static_cast<uint32_t>(st.st_mtim.tv_nsec);
    e.ctimeSec = st.st_ctim.tv_sec; e.ctimeNsec = static_cast<uint32_t>(st.st_ctim.tv_nsec);
#endif
    if (S_ISLNK(st.st_mode)) {
        std::vector<char> target(4096);
        ssize_t n = readlink(full.c_str(), target.data(), target.size());
        ensure(n >= 0, "cannot read symbolic link: " + full.string());
        while (static_cast<size_t>(n) == target.size()) {
            target.resize(target.size() * 2);
            n = readlink(full.c_str(), target.data(), target.size());
            ensure(n >= 0, "cannot read symbolic link: " + full.string());
        }
        e.linkTarget.assign(target.data(), static_cast<size_t>(n));
    }
    if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) e.device = static_cast<uint64_t>(st.st_rdev);
    return e;
}

std::vector<Entry> scanDirectory(const fs::path& source, uint64_t& inputBytes,
                                 const BackupOptions& options) {
    std::error_code ec;
    fs::path root = fs::canonical(source, ec);
    ensure(!ec && fs::is_directory(root), "source must be an existing directory");
    std::string rootName = root.filename().string();
    ensure(!rootName.empty() && rootName != "." && rootName != "..", "cannot back up filesystem root directly");

    std::vector<Entry> entries;
    entries.push_back(makeEntry(root, rootName));
    inputBytes = 0;

    fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end;
    ensure(!ec, "cannot enumerate source directory");
    for (; it != end; it.increment(ec)) {
        checkCancelled(options.cancel);
        ensure(!ec, "cannot enumerate source directory: " + ec.message());
        fs::path relative = it->path().lexically_relative(root);
        ensure(!relative.empty(), "cannot calculate source-relative path");
        std::string archivePath = (fs::path(rootName) / relative).generic_string();
        Entry entry = makeEntry(it->path(), archivePath);
        if (entry.type == EntryType::Regular) inputBytes += entry.size;
        entries.push_back(std::move(entry));
        report(options.progress, "scan", entries.size(), 0, archivePath);
    }
    ensure(entries.size() <= kMaxEntries, "source contains too many entries");
    std::sort(entries.begin() + 1, entries.end(), [](const Entry& a, const Entry& b) {
        return a.path < b.path;
    });
    return entries;
}

void writeEntryMetadata(std::ostream& out, const Entry& e, bool withOffset) {
    writeString(out, e.path);
    writeU8(out, static_cast<uint8_t>(e.type));
    writeU32(out, e.mode); writeU32(out, e.uid); writeU32(out, e.gid);
    writeU64(out, e.size);
    writeU64(out, static_cast<uint64_t>(e.atimeSec)); writeU32(out, e.atimeNsec);
    writeU64(out, static_cast<uint64_t>(e.mtimeSec)); writeU32(out, e.mtimeNsec);
    writeU64(out, static_cast<uint64_t>(e.ctimeSec)); writeU32(out, e.ctimeNsec);
    writeString(out, e.linkTarget);
    writeU64(out, e.device);
    if (withOffset) writeU64(out, e.contentOffset);
}

Entry readEntryMetadata(std::istream& in, bool withOffset) {
    Entry e;
    e.path = readString(in);
    uint8_t rawType = readU8(in);
    ensure(rawType >= static_cast<uint8_t>(EntryType::Regular) &&
           rawType <= static_cast<uint8_t>(EntryType::Socket), "invalid archive entry type");
    e.type = static_cast<EntryType>(rawType);
    e.mode = readU32(in); e.uid = readU32(in); e.gid = readU32(in);
    e.size = readU64(in);
    e.atimeSec = static_cast<int64_t>(readU64(in)); e.atimeNsec = readU32(in);
    e.mtimeSec = static_cast<int64_t>(readU64(in)); e.mtimeNsec = readU32(in);
    e.ctimeSec = static_cast<int64_t>(readU64(in)); e.ctimeNsec = readU32(in);
    e.linkTarget = readString(in);
    e.device = readU64(in);
    if (withOffset) e.contentOffset = readU64(in);
    ensure(e.atimeNsec < 1'000'000'000u && e.mtimeNsec < 1'000'000'000u &&
           e.ctimeNsec < 1'000'000'000u, "invalid nanosecond metadata");
    ensure((e.mode & ~07777u) == 0, "invalid archive permission bits");
    if (e.type != EntryType::Regular) ensure(e.size == 0, "non-regular entry has file data");
    if (e.type != EntryType::Symlink) ensure(e.linkTarget.empty(), "non-symlink entry has a link target");
    if (e.type != EntryType::Character && e.type != EntryType::Block)
        ensure(e.device == 0, "non-device entry has a device number");
    return e;
}

bool sameFileState(const struct stat& st, const Entry& e) {
    if (!S_ISREG(st.st_mode) || static_cast<uint64_t>(st.st_size) != e.size ||
        static_cast<uint64_t>(st.st_dev) != e.sourceDevice ||
        static_cast<uint64_t>(st.st_ino) != e.sourceInode) return false;
#ifdef __APPLE__
    return st.st_mtimespec.tv_sec == e.mtimeSec && static_cast<uint32_t>(st.st_mtimespec.tv_nsec) == e.mtimeNsec;
#else
    return st.st_mtim.tv_sec == e.mtimeSec && static_cast<uint32_t>(st.st_mtim.tv_nsec) == e.mtimeNsec;
#endif
}

void copySourceFile(const Entry& e, std::ostream& out, uint64_t& completed,
                    uint64_t total, const BackupOptions& options) {
    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = ::open(e.sourcePath.c_str(), flags);
    ensure(fd >= 0, "cannot securely open source file: " + e.sourcePath.string());
    struct CloseFd { int fd; ~CloseFd(){if(fd>=0)::close(fd);} } closeFd{fd};
    struct stat before{};
    ensure(fstat(fd, &before) == 0 && sameFileState(before, e),
           "source file changed after scanning: " + e.sourcePath.string());
    std::vector<char> buffer(kBufferSize);
    uint64_t remaining = e.size;
    while (remaining) {
        checkCancelled(options.cancel);
        size_t take = static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining));
        ssize_t got;
        do { got=::read(fd,buffer.data(),take); } while(got<0&&errno==EINTR);
        ensure(got > 0, "source file changed while being read: " + e.sourcePath.string());
        writeExact(out, buffer.data(), static_cast<size_t>(got));
        remaining -= static_cast<uint64_t>(got);
        completed += static_cast<uint64_t>(got);
        report(options.progress, "pack", completed, total, e.path);
    }
    struct stat openedAfter{}, pathAfter{};
    ensure(fstat(fd, &openedAfter) == 0 && sameFileState(openedAfter, e) &&
           lstat(e.sourcePath.c_str(), &pathAfter) == 0 && sameFileState(pathAfter, e),
           "source file changed while being read: " + e.sourcePath.string());
}

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

bool safeArchivePath(const std::string& value) {
    if (value.empty() || value.front() == '/' || value.find('\0') != std::string::npos) return false;
    fs::path p(value);
    if (p.is_absolute()) return false;
    for (const auto& component : p) {
        std::string s = component.string();
        if (s.empty() || s == "." || s == "..") return false;
    }
    return p.generic_string() == value;
}

void validateEntries(std::vector<Entry>& entries, uint64_t packedSize) {
    ensure(!entries.empty(), "archive contains no entries");
    ensure(entries.size() <= kMaxEntries, "archive contains too many entries");
    std::set<std::string> paths;
    for (const auto& e : entries) {
        ensure(safeArchivePath(e.path), "unsafe archive path: " + e.path);
        ensure(paths.insert(e.path).second, "duplicate archive path: " + e.path);
        if (e.type == EntryType::Regular) {
            ensure(e.contentOffset <= packedSize && e.size <= packedSize - e.contentOffset,
                   "archive entry data is outside packed stream");
        }
    }
    fs::path root(entries.front().path);
    ensure(std::distance(root.begin(), root.end()) == 1 && entries.front().type == EntryType::Directory,
           "archive does not begin with a single top-level directory");
    const std::string prefix = entries.front().path + "/";
    for (size_t i = 1; i < entries.size(); ++i) {
        ensure(entries[i].path.rfind(prefix, 0) == 0, "archive entry escapes the top-level directory");
    }
    std::map<std::string, EntryType> hierarchy;
    for (const auto& entry : entries) hierarchy.emplace(entry.path, entry.type);
    for (const auto& entry : entries) {
        fs::path parent = fs::path(entry.path).parent_path();
        while (!parent.empty()) {
            auto found = hierarchy.find(parent.generic_string());
            ensure(found != hierarchy.end(), "archive entry has a missing parent: " + entry.path);
            ensure(found->second == EntryType::Directory,
                   "archive entry is nested below a non-directory: " + entry.path);
            parent = parent.parent_path();
        }
    }
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
    static_cast<void>(readU32(in));
    ensure(centralOffset >= 8 && centralOffset <= size - 32 &&
           centralSize <= size - 32 - centralOffset, "invalid central directory bounds");
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
    ensure(static_cast<uint64_t>(in.tellg()) <= centralOffset + centralSize,
           "central directory exceeds declared size");
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

} // namespace

std::array<uint8_t, 32> sha256(const std::vector<uint8_t>& data) {
    Sha256 sha;
    if (!data.empty()) sha.update(data.data(), data.size());
    return sha.finish();
}

std::array<uint8_t, 32> sha256(const std::string& data) {
    Sha256 sha;
    if (!data.empty()) sha.update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    return sha.finish();
}

std::array<uint8_t, 32> sha256File(const std::string& path, uint64_t offset, uint64_t length) {
    return shaFileRange(path, offset, length);
}

std::string hexDigest(const std::array<uint8_t, 32>& digest) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint8_t b : digest) out << std::setw(2) << static_cast<unsigned>(b);
    return out.str();
}

namespace {

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
                   const RestoreOptions& options) {
    std::ifstream in(input, std::ios::binary);
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    ensure(in.good() && out.good(), "cannot open RLE restore stage");
    std::array<char, 4> magic{}; readExact(in, magic.data(), magic.size());
    ensure(magic == std::array<char,4>{{'R','L','E','1'}}, "invalid RLE header");
    uint64_t originalSize = readU64(in);
    uint64_t produced = 0;
    while (produced < originalSize) {
        checkCancelled(options.cancel);
        uint8_t control = readU8(in);
        size_t count = static_cast<size_t>(control & 0x7fu) + 1;
        ensure(count <= originalSize - produced, "RLE block exceeds declared output size");
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
        ensure(sum >= nodes[left].frequency, "Huffman frequency overflow");
        nodes.push_back({sum, -1, std::min(nodes[left].minimumSymbol, nodes[right].minimumSymbol), left, right});
        queue.push(static_cast<int>(nodes.size() - 1));
    }
    std::function<void(int, int)> visit = [&](int index, int depth) {
        ensure(depth <= 255, "Huffman code length exceeds format limit");
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
        for (size_t i = 0; i < got; ++i) {
            ensure(frequencies[buffer[i]] != UINT64_MAX, "Huffman frequency overflow");
            ++frequencies[buffer[i]];
        }
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
                       const RestoreOptions& options) {
    std::ifstream in(input, std::ios::binary);
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    ensure(in.good() && out.good(), "cannot open Huffman restore stage");
    std::array<char,4> magic{}; readExact(in, magic.data(), magic.size());
    ensure(magic == std::array<char,4>{{'H','U','F','1'}}, "invalid Huffman header");
    uint64_t originalSize = readU64(in);
    std::array<uint8_t,256> lengths{}; readExact(in, lengths.data(), lengths.size());
    auto codes = canonicalCodes(lengths);

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
                     CompressionAlgorithm algorithm, const RestoreOptions& options) {
    switch (algorithm) {
    case CompressionAlgorithm::None:
        copyFileStage(input, output, "decompress-copy", options.progress, options.cancel); break;
    case CompressionAlgorithm::Rle:
        rleDecompress(input, output, options); break;
    case CompressionAlgorithm::Huffman:
        huffmanDecompress(input, output, options); break;
    default: throw BackupError("unsupported compression algorithm");
    }
}

std::array<uint8_t, 32> deriveKey(const std::string& password,
                                  const std::array<uint8_t,16>& salt) {
    std::vector<uint8_t> material(password.begin(), password.end());
    material.insert(material.end(), salt.begin(), salt.end());
    auto digest = sha256(material);
    for (int round = 0; round < 4096; ++round) {
        std::vector<uint8_t> next(digest.begin(), digest.end());
        next.insert(next.end(), salt.begin(), salt.end());
        digest = sha256(next);
    }
    return digest;
}

class XorGenerator {
public:
    explicit XorGenerator(const std::array<uint8_t,32>& key) {
        for (int i = 0; i < 8; ++i) {
            a_ |= static_cast<uint64_t>(key[i]) << (i * 8);
            b_ |= static_cast<uint64_t>(key[i + 8]) << (i * 8);
        }
        if (a_ == 0 && b_ == 0) b_ = 0x9e3779b97f4a7c15ULL;
    }
    uint8_t next() {
        if (available_ == 0) {
            uint64_t x = a_, y = b_;
            a_ = y;
            x ^= x << 23;
            b_ = x ^ y ^ (x >> 17) ^ (y >> 26);
            block_ = b_ + y;
            available_ = 8;
        }
        uint8_t value = static_cast<uint8_t>(block_);
        block_ >>= 8;
        --available_;
        return value;
    }
private:
    uint64_t a_ = 0, b_ = 0, block_ = 0;
    int available_ = 0;
};

void cryptStage(const fs::path& input, const fs::path& output,
                EncryptionAlgorithm algorithm, const std::string& password,
                const std::array<uint8_t,16>& salt,
                const ProgressCallback& progress, std::atomic_bool* cancel,
                const std::string& stage) {
    ensure(algorithm == EncryptionAlgorithm::None || !password.empty(),
           "an encryption password is required");
    std::ifstream in(input, std::ios::binary);
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    ensure(in.good() && out.good(), "cannot open encryption pipeline stage");
    uint64_t total = fileSizeChecked(input), completed = 0, position = 0;
    auto key = deriveKey(password, salt);
    XorGenerator generator(key);
    std::vector<uint8_t> buffer(kBufferSize);
    while (in) {
        checkCancelled(cancel);
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        size_t got = static_cast<size_t>(in.gcount());
        if (algorithm == EncryptionAlgorithm::Xor) {
            for (size_t i = 0; i < got; ++i) buffer[i] ^= generator.next();
        } else if (algorithm == EncryptionAlgorithm::Vigenere) {
            for (size_t i = 0; i < got; ++i) {
                uint8_t k = key[(position + i) % key.size()];
                if (stage == "encrypt") buffer[i] = static_cast<uint8_t>(buffer[i] + k);
                else buffer[i] = static_cast<uint8_t>(buffer[i] - k);
            }
        } else if (algorithm != EncryptionAlgorithm::None) {
            throw BackupError("unsupported encryption algorithm");
        }
        if (got) writeExact(out, buffer.data(), got);
        position += got;
        completed += got;
        report(progress, stage, completed, total);
    }
}

std::array<uint8_t,16> randomSalt() {
    std::array<uint8_t,16> salt{};
    std::random_device random;
    for (auto& b : salt) b = static_cast<uint8_t>(random());
    return salt;
}

struct ParsedHeader {
    ArchiveInfo info;
    std::array<uint8_t,16> salt{};
};

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

bool isPathInside(const fs::path& child, const fs::path& parent) {
    auto c = child.lexically_normal(); auto p = parent.lexically_normal();
    auto ci = c.begin(), pi = p.begin();
    for (; pi != p.end(); ++pi, ++ci) {
        if (ci == c.end() || *ci != *pi) return false;
    }
    return true;
}

size_t pathDepth(const fs::path& path) {
    return static_cast<size_t>(std::distance(path.begin(), path.end()));
}

void warnMetadata(const std::string& operation, const fs::path& path) {
    std::cerr << "Warning: " << operation << " failed for " << path << ": "
              << std::strerror(errno) << '\n';
}

void rejectSymlinkParents(const fs::path& destination, const fs::path& relative) {
    fs::path current = destination;
    for (const auto& component : relative.parent_path()) {
        current /= component;
        struct stat st{};
        if (lstat(current.c_str(), &st) == 0) {
            ensure(!S_ISLNK(st.st_mode), "restore parent is a symbolic link: " + current.string());
            ensure(S_ISDIR(st.st_mode), "restore parent is not a directory: " + current.string());
        } else {
            ensure(errno == ENOENT, "cannot inspect restore parent: " + current.string());
        }
    }
}

void checkTargetCompatibility(const fs::path& target, const Entry& entry, bool overwrite) {
    struct stat st{};
    if (lstat(target.c_str(), &st) != 0) {
        ensure(errno == ENOENT, "cannot inspect restore target: " + target.string());
        return;
    }
    ensure(overwrite, "restore target already exists: " + target.string());
    if (entry.type == EntryType::Directory) {
        ensure(S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode),
               "cannot overwrite non-directory with directory: " + target.string());
    } else {
        ensure(!S_ISDIR(st.st_mode), "cannot overwrite directory with non-directory: " + target.string());
    }
}

void removeExistingNonDirectory(const fs::path& target) {
    struct stat st{};
    if (lstat(target.c_str(), &st) == 0) {
        ensure(!S_ISDIR(st.st_mode), "refusing to remove directory during restore: " + target.string());
        ensure(unlink(target.c_str()) == 0, "cannot replace restore target: " + target.string());
    } else {
        ensure(errno == ENOENT, "cannot inspect restore target: " + target.string());
    }
}

void restoreSocketNode(const fs::path& path) {
    ensure(path.string().size() < sizeof(sockaddr_un::sun_path), "Unix socket path is too long");
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ensure(fd >= 0, "cannot create Unix socket node");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    int result = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    int saved = errno;
    ::close(fd);
    errno = saved;
    ensure(result == 0, "cannot bind restored Unix socket node: " + path.string());
}

void applyMetadata(const fs::path& target, const Entry& entry) {
    if (lchown(target.c_str(), static_cast<uid_t>(entry.uid), static_cast<gid_t>(entry.gid)) != 0 &&
        errno != EPERM && errno != EACCES) warnMetadata("lchown", target);
    if (entry.type != EntryType::Symlink) {
        if (chmod(target.c_str(), static_cast<mode_t>(entry.mode)) != 0) warnMetadata("chmod", target);
    }
    timespec times[2]{};
    times[0].tv_sec = static_cast<time_t>(entry.atimeSec);
    times[0].tv_nsec = static_cast<long>(entry.atimeNsec);
    times[1].tv_sec = static_cast<time_t>(entry.mtimeSec);
    times[1].tv_nsec = static_cast<long>(entry.mtimeNsec);
    if (utimensat(AT_FDCWD, target.c_str(), times,
                  entry.type == EntryType::Symlink ? AT_SYMLINK_NOFOLLOW : 0) != 0 &&
        errno != EPERM && errno != EACCES && errno != ENOTSUP) warnMetadata("utimensat", target);
}

void extractEntries(const fs::path& packed, const fs::path& destination,
                    std::vector<Entry> entries, const RestoreOptions& options,
                    uint64_t& outputBytes) {
    std::error_code ec;
    if (fs::exists(destination, ec)) {
        ensure(!ec && fs::is_directory(destination) && !fs::is_symlink(destination),
               "restore destination must be a real directory");
    } else {
        ensure(fs::create_directories(destination, ec) && !ec,
               "cannot create restore destination");
    }
    fs::path canonicalDestination = fs::canonical(destination, ec);
    ensure(!ec, "cannot canonicalize restore destination");

    for (const auto& entry : entries) {
        fs::path relative(entry.path);
        rejectSymlinkParents(canonicalDestination, relative);
        checkTargetCompatibility(canonicalDestination / relative, entry, options.overwrite);
    }

    std::vector<const Entry*> directories;
    for (const auto& entry : entries) if (entry.type == EntryType::Directory) directories.push_back(&entry);
    std::sort(directories.begin(), directories.end(), [](const Entry* a, const Entry* b) {
        return pathDepth(a->path) < pathDepth(b->path);
    });
    for (const Entry* entry : directories) {
        checkCancelled(options.cancel);
        fs::path target = canonicalDestination / fs::path(entry->path);
        if (!fs::exists(target, ec)) {
            ensure(fs::create_directory(target, ec) && !ec, "cannot create directory: " + target.string());
        }
    }

    std::ifstream data(packed, std::ios::binary);
    ensure(data.good(), "cannot reopen packed data for extraction");
    outputBytes = 0;
    for (const auto& entry : entries) {
        checkCancelled(options.cancel);
        if (entry.type == EntryType::Directory) continue;
        fs::path target = canonicalDestination / fs::path(entry.path);
        if (entry.type == EntryType::Regular) {
            TempFile temp(target.parent_path());
            std::ofstream out(temp.path(), std::ios::binary | std::ios::trunc);
            ensure(out.good(), "cannot create restored file: " + target.string());
            data.clear();
            data.seekg(static_cast<std::streamoff>(entry.contentOffset));
            ensure(data.good(), "cannot seek to packed file content");
            copyBytes(data, out, entry.size, options.progress, "extract", options.cancel);
            out.close();
            removeExistingNonDirectory(target);
            fs::rename(temp.path(), target, ec);
            ensure(!ec, "cannot commit restored file: " + target.string());
            temp.keep();
            outputBytes += entry.size;
        } else if (entry.type == EntryType::Symlink) {
            removeExistingNonDirectory(target);
            ensure(::symlink(entry.linkTarget.c_str(), target.c_str()) == 0,
                   "cannot restore symbolic link: " + target.string());
        } else if (entry.type == EntryType::Fifo) {
            removeExistingNonDirectory(target);
            ensure(::mkfifo(target.c_str(), static_cast<mode_t>(entry.mode)) == 0,
                   "cannot restore FIFO: " + target.string());
        } else if (entry.type == EntryType::Character || entry.type == EntryType::Block) {
            removeExistingNonDirectory(target);
            mode_t type = entry.type == EntryType::Character ? S_IFCHR : S_IFBLK;
            ensure(::mknod(target.c_str(), type | static_cast<mode_t>(entry.mode),
                           static_cast<dev_t>(entry.device)) == 0,
                   "cannot restore device node (root permission may be required): " + target.string());
        } else if (entry.type == EntryType::Socket) {
            removeExistingNonDirectory(target);
            restoreSocketNode(target);
        }
        report(options.progress, "restore-entry", outputBytes, 0, entry.path);
    }

    for (const auto& entry : entries) {
        if (entry.type != EntryType::Directory) applyMetadata(canonicalDestination / entry.path, entry);
    }
    std::sort(directories.begin(), directories.end(), [](const Entry* a, const Entry* b) {
        return pathDepth(a->path) > pathDepth(b->path);
    });
    for (const Entry* entry : directories) applyMetadata(canonicalDestination / entry->path, *entry);
}

using DecodedArchiveConsumer =
    std::function<void(const ParsedHeader&, const fs::path&, const std::vector<Entry>&)>;

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
    decompressStage(compressed.path(), packed.path(), header.info.compression, options);
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

} // namespace

BackupResult BackupEngine::create(const std::string& sourceDirectory,
                                  const std::string& archivePath,
                                  const BackupOptions& options) {
    BackupResult result;
    try {
        if (options.encryption != EncryptionAlgorithm::None) {
            ensure(!options.password.empty(), "an encryption password is required");
        }
        fs::path source = fs::canonical(sourceDirectory);
        fs::path output = fs::absolute(archivePath).lexically_normal();
        fs::path outputParent = output.parent_path().empty() ? fs::current_path() : output.parent_path();
        std::error_code ec;
        fs::create_directories(outputParent, ec);
        ensure(!ec, "cannot create archive output directory");
        fs::path canonicalParent = fs::canonical(outputParent, ec);
        ensure(!ec, "cannot canonicalize archive output directory");
        fs::path canonicalOutput = canonicalParent / output.filename();
        ensure(!isPathInside(canonicalOutput, source), "archive output cannot be inside the source directory");
        struct stat outputState{};
        ensure(lstat(canonicalOutput.c_str(), &outputState) != 0,
               "archive output already exists");
        ensure(errno == ENOENT, "cannot inspect archive output path");

        uint64_t inputBytes = 0;
        auto entries = scanDirectory(source, inputBytes, options);
        TempFile packed;
        if (options.pack == PackAlgorithm::Stream) packStream(entries, packed.path(), inputBytes, options);
        else if (options.pack == PackAlgorithm::Index) packIndex(entries, packed.path(), inputBytes, options);
        else throw BackupError("unsupported pack algorithm");

        ArchiveInfo info;
        info.version = kArchiveVersion;
        info.pack = options.pack;
        info.compression = options.compression;
        info.encryption = options.encryption;
        info.packedSize = fileSizeChecked(packed.path());
        info.packedDigest = shaFileRange(packed.path(), 0, UINT64_MAX);

        TempFile compressed;
        compressStage(packed.path(), compressed.path(), options.compression, options);
        auto salt = randomSalt();
        TempFile encoded;
        cryptStage(compressed.path(), encoded.path(), options.encryption, options.password,
                   salt, options.progress, options.cancel, "encrypt");
        info.encodedSize = fileSizeChecked(encoded.path());
        info.encodedDigest = shaFileRange(encoded.path(), 0, UINT64_MAX);

        TempFile finalTemp(canonicalParent);
        std::ofstream out(finalTemp.path(), std::ios::binary | std::ios::trunc);
        ensure(out.good(), "cannot create final archive");
        writeArchiveHeader(out, info, salt);
        std::ifstream payload(encoded.path(), std::ios::binary);
        ensure(payload.good(), "cannot reopen encoded payload");
        copyBytes(payload, out, info.encodedSize, options.progress, "finalize", options.cancel);
        out.close();
        int fd = ::open(finalTemp.path().c_str(), O_RDONLY);
        if (fd >= 0) { static_cast<void>(::fsync(fd)); ::close(fd); }
        commitFileNoReplace(finalTemp.path(), canonicalOutput);

        result.success = true;
        result.message = "backup completed";
        result.entryCount = entries.size();
        result.inputBytes = inputBytes;
        result.outputBytes = kArchiveHeaderSize + info.encodedSize;
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
        withDecodedArchive(archive, options, [&](const ParsedHeader&, const fs::path& packed,
                                                 const std::vector<Entry>& entries) {
            extractEntries(packed, fs::absolute(destinationDirectory), entries, options, outputBytes);
            result.entryCount = entries.size();
        });
        result.success = true;
        result.message = "restore completed";
        result.inputBytes = fileSizeChecked(archive);
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
    withDecodedArchive(archive, options, [&](const ParsedHeader&, const fs::path&,
                                             const std::vector<Entry>& entries) {
        fs::path destination = fs::absolute(destinationDirectory).lexically_normal();
        std::error_code ec;
        if (fs::exists(destination, ec)) {
            ensure(!ec && fs::is_directory(destination) && !fs::is_symlink(destination),
                   "restore destination must be a real directory");
            destination = fs::canonical(destination, ec);
            ensure(!ec, "cannot canonicalize restore destination");
        } else {
            ensure(!ec, "cannot inspect restore destination");
        }
        preview.entries.reserve(entries.size());
        for (const auto& entry : entries) {
            preview.entries.push_back({entry.path, entryTypeName(entry.type), entry.size});
            struct stat st{};
            fs::path target = destination / fs::path(entry.path);
            if (lstat(target.c_str(), &st) == 0) preview.conflicts.push_back(entry.path);
            else ensure(errno == ENOENT, "cannot inspect restore target: " + target.string());
        }
    });
    return preview;
}

ArchiveInfo BackupEngine::inspect(const std::string& archivePath) {
    fs::path archive = fs::canonical(archivePath);
    ParsedHeader header = readArchiveHeader(archive);
    ensure(shaFileRange(archive, kArchiveHeaderSize, header.info.encodedSize) == header.info.encodedDigest,
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
