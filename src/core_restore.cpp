#include "core_internal.hpp"

namespace backup {
namespace detail {

class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) : fd_(fd) {}
    ~UniqueFd() { if (fd_ >= 0) ::close(fd_); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    int get() const { return fd_; }
    int release() { int value = fd_; fd_ = -1; return value; }
private:
    int fd_;
};

UniqueFd openDirectoryAt(int rootFd, const fs::path& relative) {
    UniqueFd current(::openat(rootFd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    ensure(current.get() >= 0, "cannot duplicate restore directory descriptor");
    for (const auto& component : relative) {
        std::string name = component.string();
        int fd = ::openat(current.get(), name.c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ensure(fd >= 0, "restore parent is not a real directory: " + relative.string());
        current = UniqueFd(fd);
    }
    return current;
}

UniqueFd openParentDirectory(int rootFd, const std::string& archivePath) {
    return openDirectoryAt(rootFd, fs::path(archivePath).parent_path());
}

std::string temporaryNodeName() {
    static std::atomic_uint64_t sequence{0};
    std::random_device random;
    std::ostringstream out;
    out << ".backup-tmp-" << std::hex << random() << random() << sequence.fetch_add(1);
    return out.str();
}

class TemporaryNode {
public:
    TemporaryNode(int parentFd, std::string name)
        : parentFd_(parentFd), name_(std::move(name)) {}
    ~TemporaryNode() {
        if (!keep_) static_cast<void>(::unlinkat(parentFd_, name_.c_str(), 0));
    }
    const std::string& name() const { return name_; }
    void keep() { keep_ = true; }
private:
    int parentFd_;
    std::string name_;
    bool keep_ = false;
};

std::pair<UniqueFd, std::string> createTemporaryFileAt(int parentFd) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::string name = temporaryNodeName();
        int fd = ::openat(parentFd, name.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) return {UniqueFd(fd), std::move(name)};
        ensure(errno == EEXIST, "cannot create secure restore temporary file");
    }
    throw BackupError("cannot allocate a unique restore temporary file");
}

template <typename Creator>
std::string createTemporaryNodeAt(Creator create) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::string name = temporaryNodeName();
        if (create(name) == 0) return name;
        ensure(errno == EEXIST || errno == EADDRINUSE,
               "cannot create secure restore temporary node");
    }
    throw BackupError("cannot allocate a unique restore temporary node");
}

void commitNodeAt(int parentFd, const std::string& temporary,
                  const std::string& target, bool overwrite) {
    if (overwrite) {
        ensure(::renameat(parentFd, temporary.c_str(), parentFd, target.c_str()) == 0,
               "cannot atomically replace restore target: " + std::string(std::strerror(errno)));
        return;
    }
#if defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, parentFd, temporary.c_str(), parentFd,
                  target.c_str(), RENAME_NOREPLACE) == 0) return;
    if (errno != ENOSYS && errno != EINVAL)
        throw BackupError("restore target already exists or cannot be committed: " +
                          std::string(std::strerror(errno)));
#endif
    ensure(::linkat(parentFd, temporary.c_str(), parentFd, target.c_str(), 0) == 0,
           "restore target already exists or cannot be committed: " +
           std::string(std::strerror(errno)));
    ensure(::unlinkat(parentFd, temporary.c_str(), 0) == 0,
           "cannot remove committed restore temporary node");
}

void applyMetadataAt(int parentFd, const std::string& name,
                     const Entry& entry, const fs::path& displayPath) {
    if (::fchownat(parentFd, name.c_str(), static_cast<uid_t>(entry.uid),
                   static_cast<gid_t>(entry.gid), AT_SYMLINK_NOFOLLOW) != 0 &&
        errno != EPERM && errno != EACCES) {
        warnMetadata("fchownat", displayPath);
    }
    if (entry.type != EntryType::Symlink &&
        ::fchmodat(parentFd, name.c_str(), static_cast<mode_t>(entry.mode),
                   AT_SYMLINK_NOFOLLOW) != 0) {
        warnMetadata("fchmodat", displayPath);
    }
    timespec times[2]{
        {static_cast<time_t>(entry.atimeSec), static_cast<long>(entry.atimeNsec)},
        {static_cast<time_t>(entry.mtimeSec), static_cast<long>(entry.mtimeNsec)}
    };
    if (::utimensat(parentFd, name.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0 &&
        errno != EPERM && errno != EACCES && errno != ENOTSUP) {
        warnMetadata("utimensat", displayPath);
    }
}

void copyPackedFileToFd(std::ifstream& input, int outputFd, uint64_t size,
                        const Entry& entry, const RestoreOptions& options) {
    input.clear();
    input.seekg(static_cast<std::streamoff>(entry.contentOffset));
    ensure(input.good(), "cannot seek to packed file content");
    std::vector<char> buffer(kBufferSize);
    uint64_t completed = 0;
    while (completed < size) {
        checkCancelled(options.cancel);
        size_t wanted = static_cast<size_t>(
            std::min<uint64_t>(buffer.size(), size - completed));
        input.read(buffer.data(), static_cast<std::streamsize>(wanted));
        ensure(static_cast<size_t>(input.gcount()) == wanted,
               "truncated packed file content");
        size_t written = 0;
        while (written < wanted) {
            ssize_t count = ::write(outputFd, buffer.data() + written, wanted - written);
            if (count < 0 && errno == EINTR) continue;
            ensure(count > 0, "cannot write restored file");
            written += static_cast<size_t>(count);
        }
        completed += wanted;
        report(options.progress, "extract", completed, size, entry.path);
    }
}

void ensureDirectoryAt(int parentFd, const std::string& name, bool overwrite,
                       const fs::path& displayPath) {
    if (::mkdirat(parentFd, name.c_str(), 0700) == 0) return;
    ensure(errno == EEXIST, "cannot create directory: " + displayPath.string());
    struct stat state{};
    ensure(::fstatat(parentFd, name.c_str(), &state, AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISDIR(state.st_mode) && overwrite,
           "restore directory already exists or is not a real directory: " +
           displayPath.string());
}

void createSocketNodeAt(int parentFd, const std::string& name) {
    std::string path = "/proc/self/fd/" + std::to_string(parentFd) + "/" + name;
    ensure(path.size() < sizeof(sockaddr_un::sun_path),
           "Unix socket restore path is too long");
    UniqueFd socketFd(::socket(AF_UNIX, SOCK_STREAM, 0));
    ensure(socketFd.get() >= 0, "cannot create Unix socket node");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    ensure(::bind(socketFd.get(), reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) == 0,
           "cannot bind restored Unix socket node");
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
    UniqueFd destinationFd(::open(canonicalDestination.c_str(),
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    ensure(destinationFd.get() >= 0, "cannot securely open restore destination");

    std::vector<const Entry*> directories;
    for (const auto& entry : entries) if (entry.type == EntryType::Directory) directories.push_back(&entry);
    std::sort(directories.begin(), directories.end(), [](const Entry* a, const Entry* b) {
        return pathDepth(a->path) < pathDepth(b->path);
    });
    for (const Entry* entry : directories) {
        checkCancelled(options.cancel);
        UniqueFd parent = openParentDirectory(destinationFd.get(), entry->path);
        std::string name = fs::path(entry->path).filename().string();
        ensureDirectoryAt(parent.get(), name, options.overwrite,
                          canonicalDestination / entry->path);
    }

    std::ifstream data(packed, std::ios::binary);
    ensure(data.good(), "cannot reopen packed data for extraction");
    outputBytes = 0;
    for (const auto& entry : entries) {
        checkCancelled(options.cancel);
        if (entry.type == EntryType::Directory) continue;
        UniqueFd parent = openParentDirectory(destinationFd.get(), entry.path);
        std::string target = fs::path(entry.path).filename().string();
        fs::path displayPath = canonicalDestination / entry.path;
        std::string temporary;
        UniqueFd regularFd;
        if (entry.type == EntryType::Regular) {
            auto created = createTemporaryFileAt(parent.get());
            regularFd = std::move(created.first);
            temporary = std::move(created.second);
        } else if (entry.type == EntryType::Symlink) {
            temporary = createTemporaryNodeAt([&](const std::string& name) {
                return ::symlinkat(entry.linkTarget.c_str(), parent.get(), name.c_str());
            });
        } else if (entry.type == EntryType::Fifo) {
            temporary = createTemporaryNodeAt([&](const std::string& name) {
                return ::mkfifoat(parent.get(), name.c_str(), 0600);
            });
        } else if (entry.type == EntryType::Character || entry.type == EntryType::Block) {
            mode_t type = entry.type == EntryType::Character ? S_IFCHR : S_IFBLK;
            temporary = createTemporaryNodeAt([&](const std::string& name) {
                return ::mknodat(parent.get(), name.c_str(), type | 0600,
                                 static_cast<dev_t>(entry.device));
            });
        } else if (entry.type == EntryType::Socket) {
            temporary = createTemporaryNodeAt([&](const std::string& name) {
                try {
                    createSocketNodeAt(parent.get(), name);
                    return 0;
                } catch (...) {
                    if (errno == EADDRINUSE) return -1;
                    throw;
                }
            });
        }
        TemporaryNode cleanup(parent.get(), temporary);
        if (entry.type == EntryType::Regular) {
            copyPackedFileToFd(data, regularFd.get(), entry.size, entry, options);
        }
        applyMetadataAt(parent.get(), temporary, entry, displayPath);
        if (entry.type == EntryType::Regular) {
            ensure(::fsync(regularFd.get()) == 0, "cannot synchronize restored file");
            ensure(::close(regularFd.release()) == 0, "cannot close restored file");
            outputBytes += entry.size;
        }
        commitNodeAt(parent.get(), temporary, target, options.overwrite);
        cleanup.keep();
        report(options.progress, "restore-entry", outputBytes, 0, entry.path);
    }

    std::sort(directories.begin(), directories.end(), [](const Entry* a, const Entry* b) {
        return pathDepth(a->path) > pathDepth(b->path);
    });
    for (const Entry* entry : directories) {
        UniqueFd parent = openParentDirectory(destinationFd.get(), entry->path);
        std::string name = fs::path(entry->path).filename().string();
        applyMetadataAt(parent.get(), name, *entry, canonicalDestination / entry->path);
    }
}
} // namespace detail
} // namespace backup
