#include "internal.hpp"

namespace backup {
namespace detail {

EntryType entryTypeFromMode(mode_t mode) {
    if (S_ISREG(mode))
        return EntryType::Regular;
    if (S_ISDIR(mode))
        return EntryType::Directory;
    if (S_ISLNK(mode))
        return EntryType::Symlink;
    if (S_ISFIFO(mode))
        return EntryType::Fifo;
    if (S_ISCHR(mode))
        return EntryType::Character;
    if (S_ISBLK(mode))
        return EntryType::Block;
    if (S_ISSOCK(mode))
        return EntryType::Socket;
    throw BackupError("unsupported filesystem entry type");
}

Entry makeEntry(const fs::path& full, const std::string& archivePath) {
    struct stat st {};
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
    e.atimeSec = st.st_atimespec.tv_sec;
    e.atimeNsec = static_cast<uint32_t>(st.st_atimespec.tv_nsec);
    e.mtimeSec = st.st_mtimespec.tv_sec;
    e.mtimeNsec = static_cast<uint32_t>(st.st_mtimespec.tv_nsec);
    e.ctimeSec = st.st_ctimespec.tv_sec;
    e.ctimeNsec = static_cast<uint32_t>(st.st_ctimespec.tv_nsec);
#else
    e.atimeSec = st.st_atim.tv_sec;
    e.atimeNsec = static_cast<uint32_t>(st.st_atim.tv_nsec);
    e.mtimeSec = st.st_mtim.tv_sec;
    e.mtimeNsec = static_cast<uint32_t>(st.st_mtim.tv_nsec);
    e.ctimeSec = st.st_ctim.tv_sec;
    e.ctimeNsec = static_cast<uint32_t>(st.st_ctim.tv_nsec);
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
    if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))
        e.device = static_cast<uint64_t>(st.st_rdev);
    return e;
}

std::vector<Entry> scanDirectory(const fs::path& source, uint64_t& inputBytes,
                                 const BackupOptions& options) {
    std::error_code ec;
    fs::path root = fs::canonical(source, ec);
    ensure(!ec && fs::is_directory(root), "source must be an existing directory");
    std::string rootName = root.filename().string();
    ensure(!rootName.empty() && rootName != "." && rootName != "..",
           "cannot back up filesystem root directly");

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
        if (entry.type == EntryType::Regular)
            inputBytes += entry.size;
        entries.push_back(std::move(entry));
        report(options.progress, "scan", entries.size(), 0, archivePath);
    }
    ensure(entries.size() <= kMaxEntries, "source contains too many entries");
    std::sort(entries.begin() + 1, entries.end(),
              [](const Entry& a, const Entry& b) { return a.path < b.path; });
    return entries;
}

void writeEntryMetadata(std::ostream& out, const Entry& e, bool withOffset) {
    writeString(out, e.path);
    writeU8(out, static_cast<uint8_t>(e.type));
    writeU32(out, e.mode);
    writeU32(out, e.uid);
    writeU32(out, e.gid);
    writeU64(out, e.size);
    writeU64(out, static_cast<uint64_t>(e.atimeSec));
    writeU32(out, e.atimeNsec);
    writeU64(out, static_cast<uint64_t>(e.mtimeSec));
    writeU32(out, e.mtimeNsec);
    writeU64(out, static_cast<uint64_t>(e.ctimeSec));
    writeU32(out, e.ctimeNsec);
    writeString(out, e.linkTarget);
    writeU64(out, e.device);
    if (withOffset)
        writeU64(out, e.contentOffset);
}

Entry readEntryMetadata(std::istream& in, bool withOffset) {
    Entry e;
    e.path = readString(in);
    uint8_t rawType = readU8(in);
    ensure(rawType >= static_cast<uint8_t>(EntryType::Regular) &&
               rawType <= static_cast<uint8_t>(EntryType::Socket),
           "invalid archive entry type");
    e.type = static_cast<EntryType>(rawType);
    e.mode = readU32(in);
    e.uid = readU32(in);
    e.gid = readU32(in);
    e.size = readU64(in);
    e.atimeSec = static_cast<int64_t>(readU64(in));
    e.atimeNsec = readU32(in);
    e.mtimeSec = static_cast<int64_t>(readU64(in));
    e.mtimeNsec = readU32(in);
    e.ctimeSec = static_cast<int64_t>(readU64(in));
    e.ctimeNsec = readU32(in);
    e.linkTarget = readString(in);
    e.device = readU64(in);
    if (withOffset)
        e.contentOffset = readU64(in);
    ensure(e.atimeNsec < 1'000'000'000u && e.mtimeNsec < 1'000'000'000u &&
               e.ctimeNsec < 1'000'000'000u,
           "invalid nanosecond metadata");
    ensure((e.mode & ~07777u) == 0, "invalid archive permission bits");
    if (e.type != EntryType::Regular)
        ensure(e.size == 0, "non-regular entry has file data");
    if (e.type != EntryType::Symlink)
        ensure(e.linkTarget.empty(), "non-symlink entry has a link target");
    if (e.type != EntryType::Character && e.type != EntryType::Block)
        ensure(e.device == 0, "non-device entry has a device number");
    return e;
}

bool sameFileState(const struct stat& st, const Entry& e) {
    if (!S_ISREG(st.st_mode) || static_cast<uint64_t>(st.st_size) != e.size ||
        static_cast<uint64_t>(st.st_dev) != e.sourceDevice ||
        static_cast<uint64_t>(st.st_ino) != e.sourceInode)
        return false;
#ifdef __APPLE__
    return st.st_mtimespec.tv_sec == e.mtimeSec &&
           static_cast<uint32_t>(st.st_mtimespec.tv_nsec) == e.mtimeNsec;
#else
    return st.st_mtim.tv_sec == e.mtimeSec &&
           static_cast<uint32_t>(st.st_mtim.tv_nsec) == e.mtimeNsec;
#endif
}

void copySourceFile(const Entry& e, std::ostream& out, uint64_t& completed, uint64_t total,
                    const BackupOptions& options) {
    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = ::open(e.sourcePath.c_str(), flags);
    ensure(fd >= 0, "cannot securely open source file: " + e.sourcePath.string());
    struct CloseFd {
        int fd;
        ~CloseFd() {
            if (fd >= 0)
                ::close(fd);
        }
    } closeFd{fd};
    struct stat before {};
    ensure(fstat(fd, &before) == 0 && sameFileState(before, e),
           "source file changed after scanning: " + e.sourcePath.string());
    std::vector<char> buffer(kBufferSize);
    uint64_t remaining = e.size;
    while (remaining) {
        checkCancelled(options.cancel);
        size_t take = static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining));
        ssize_t got;
        do {
            got = ::read(fd, buffer.data(), take);
        } while (got < 0 && errno == EINTR);
        ensure(got > 0, "source file changed while being read: " + e.sourcePath.string());
        writeExact(out, buffer.data(), static_cast<size_t>(got));
        remaining -= static_cast<uint64_t>(got);
        completed += static_cast<uint64_t>(got);
        report(options.progress, "pack", completed, total, e.path);
    }
    struct stat openedAfter {
    }, pathAfter{};
    ensure(fstat(fd, &openedAfter) == 0 && sameFileState(openedAfter, e) &&
               lstat(e.sourcePath.c_str(), &pathAfter) == 0 && sameFileState(pathAfter, e),
           "source file changed while being read: " + e.sourcePath.string());
}

bool safeArchivePath(const std::string& value) {
    if (value.empty() || value.find('\0') != std::string::npos)
        return false;
    fs::path p(value);
    if (p.is_absolute())
        return false;
    for (const auto& component : p) {
        std::string s = component.string();
        if (s.empty() || s == "." || s == "..")
            return false;
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
    ensure(std::distance(root.begin(), root.end()) == 1 &&
               entries.front().type == EntryType::Directory,
           "archive does not begin with a single top-level directory");
    const std::string prefix = entries.front().path + "/";
    for (size_t i = 1; i < entries.size(); ++i) {
        ensure(entries[i].path.rfind(prefix, 0) == 0,
               "archive entry escapes the top-level directory");
    }
    std::map<std::string, EntryType> hierarchy;
    for (const auto& entry : entries)
        hierarchy.emplace(entry.path, entry.type);
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
} // namespace detail
} // namespace backup
