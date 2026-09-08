#include "helpers.hpp"

namespace fs = std::filesystem;
using namespace backup;

bool gSocketFixtureAvailable = false;

void check(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

TempDirectory::TempDirectory() {
    std::string pattern = (fs::temp_directory_path() / "backup-tests-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* created = ::mkdtemp(buffer.data());
    check(created != nullptr, "mkdtemp failed");
    path = created;
}

TempDirectory::~TempDirectory() {
    std::error_code error;
    fs::remove_all(path, error);
}

void writeBytes(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    check(out.good(), "cannot write fixture: " + path.string());
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    check(out.good(), "cannot finish fixture: " + path.string());
}

std::vector<uint8_t> readBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    check(in.good(), "cannot read file: " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void makeSocketNode(const fs::path& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    check(fd >= 0, "socket fixture failed");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    check(path.string().size() < sizeof(address.sun_path), "socket fixture path too long");
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    int result = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    int error = errno;
    ::close(fd);
    if (result == 0) {
        gSocketFixtureAvailable = true;
        return;
    }
    if (error == EPERM || error == EACCES) {
        std::cout << "SKIP: Unix socket nodes are blocked by this sandbox\n";
        return;
    }
    check(false, "socket fixture bind failed: " + std::string(std::strerror(error)));
}

void createFixture(const fs::path& root) {
    fs::create_directories(root / "nested" / "emptydir");
    fs::create_directories(root / "中文目录");
    writeBytes(root / "empty.bin", {});
    writeBytes(root / "hello.txt", {'h', 'e', 'l', 'l', 'o', '\n'});
    writeBytes(root / "executable.sh", {'#', '!', '/', 'b', 'i', 'n', '/', 's', 'h', '\n'});
    check(::chmod((root / "executable.sh").c_str(), 0751) == 0, "chmod fixture failed");

    std::vector<uint8_t> allBytes(256);
    for (int value = 0; value < 256; ++value)
        allBytes[value] = static_cast<uint8_t>(value);
    writeBytes(root / "all-bytes.bin", allBytes);

    std::vector<uint8_t> randomData(8192);
    uint32_t state = 0x13579bdfu;
    for (auto& byte : randomData) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<uint8_t>(state);
    }
    writeBytes(root / "random.bin", randomData);

    std::vector<uint8_t> rleBoundary;
    rleBoundary.insert(rleBoundary.end(), 127, 'A');
    rleBoundary.insert(rleBoundary.end(), 128, 'B');
    rleBoundary.insert(rleBoundary.end(), 2, 'C');
    rleBoundary.insert(rleBoundary.end(), 3, 'D');
    rleBoundary.insert(rleBoundary.end(), 129, 'E');
    writeBytes(root / "rle-boundaries.bin", rleBoundary);
    writeBytes(root / "nested" / "single-symbol.bin", std::vector<uint8_t>(4096, 0x41));
    writeBytes(root / "中文目录" / "数据.txt", {'U', 'T', 'F', '-', '8', '\n'});

    fs::path deep = root;
    for (int depth = 0; depth < 8; ++depth) {
        deep /= "depth-" + std::to_string(depth);
        fs::create_directory(deep);
    }
    writeBytes(deep / (std::string(180, 'x') + ".bin"), {'l', 'o', 'n', 'g', '\n'});

    check(::chmod((root / "hello.txt").c_str(), 0600) == 0, "file mode fixture failed");
    timespec executableTimes[2]{{1700000000, 123456789}, {1700000001, 987654321}};
    check(::utimensat(AT_FDCWD, (root / "executable.sh").c_str(), executableTimes, 0) == 0,
          "timestamp fixture failed");
    check(::symlink("../hello.txt", (root / "nested" / "hello-link").c_str()) == 0,
          "symlink fixture failed");
    check(::mkfifo((root / "named-pipe").c_str(), 0640) == 0, "FIFO fixture failed");
    makeSocketNode(root / "unix-socket");
}

std::vector<std::string> listTree(const fs::path& root) {
    std::vector<std::string> paths{"."};
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::none, error), end;
         it != end; it.increment(error)) {
        check(!error, "cannot enumerate fixture tree");
        paths.push_back(it->path().lexically_relative(root).generic_string());
    }
    check(!error, "cannot finish fixture enumeration");
    std::sort(paths.begin(), paths.end());
    return paths;
}

void normalizeAtimes(const fs::path& root, const std::vector<std::string>& paths) {
    for (size_t index = 0; index < paths.size(); ++index) {
        fs::path path = paths[index] == "." ? root : root / paths[index];
        struct stat state {};
        check(::lstat(path.c_str(), &state) == 0, "cannot stat fixture for atime normalization");
        timespec times[2]{{1700001000 + static_cast<time_t>(index),
                           static_cast<long>((index * 7919) % 1000000000)},
                          state.st_mtim};
        int flags = S_ISLNK(state.st_mode) ? AT_SYMLINK_NOFOLLOW : 0;
        check(::utimensat(AT_FDCWD, path.c_str(), times, flags) == 0,
              "cannot normalize fixture atime");
    }
}

std::vector<uint8_t> readWithoutAtime(const fs::path& path, uint64_t size) {
    int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
#ifdef O_NOATIME
    flags |= O_NOATIME;
#endif
    int fd = ::open(path.c_str(), flags);
    check(fd >= 0, "cannot securely read tree entry: " + path.string());
    std::vector<uint8_t> data(static_cast<size_t>(size));
    size_t done = 0;
    while (done < data.size()) {
        ssize_t count = ::read(fd, data.data() + done, data.size() - done);
        if (count < 0 && errno == EINTR)
            continue;
        check(count > 0, "truncated tree entry: " + path.string());
        done += static_cast<size_t>(count);
    }
    check(::close(fd) == 0, "cannot close tree entry");
    return data;
}

TreeSnapshot snapshotKnownTree(const fs::path& root,
                               const std::vector<std::string>& expectedPaths) {
    TreeSnapshot snapshot;
    for (const std::string& relative : expectedPaths) {
        fs::path path = relative == "." ? root : root / relative;
        struct stat state {};
        check(::lstat(path.c_str(), &state) == 0, "tree entry is missing: " + relative);
        TreeEntry entry;
        entry.type = state.st_mode & S_IFMT;
        entry.mode = state.st_mode & 07777;
        entry.uid = state.st_uid;
        entry.gid = state.st_gid;
        entry.size = S_ISREG(state.st_mode) ? static_cast<uint64_t>(state.st_size) : 0;
        entry.atime = state.st_atim;
        entry.mtime = state.st_mtim;
        if (S_ISREG(state.st_mode))
            entry.content = readWithoutAtime(path, entry.size);
        if (S_ISLNK(state.st_mode)) {
            std::vector<char> target(4096);
            ssize_t length = ::readlink(path.c_str(), target.data(), target.size());
            check(length >= 0 && static_cast<size_t>(length) < target.size(),
                  "cannot read tree symlink");
            entry.linkTarget.assign(target.data(), static_cast<size_t>(length));
            timespec preserve[2]{state.st_atim, state.st_mtim};
            check(::utimensat(AT_FDCWD, path.c_str(), preserve, AT_SYMLINK_NOFOLLOW) == 0,
                  "cannot preserve symlink timestamps while comparing");
        }
        snapshot.emplace(relative, std::move(entry));
    }
    return snapshot;
}

bool sameTime(const timespec& left, const timespec& right) {
    return left.tv_sec == right.tv_sec && left.tv_nsec == right.tv_nsec;
}

void compareCompleteTree(const fs::path& restored, const std::vector<std::string>& expectedPaths,
                         const TreeSnapshot& expected) {
    TreeSnapshot actual = snapshotKnownTree(restored, expectedPaths);
    check(listTree(restored) == expectedPaths, "restored tree has missing or unexpected entries");
    check(actual.size() == expected.size(), "restored tree entry count mismatch");
    for (const auto& [path, source] : expected) {
        const TreeEntry& target = actual.at(path);
        check(target.type == source.type, "entry type mismatch: " + path);
        check(target.mode == source.mode, "entry mode mismatch: " + path);
        check(target.uid == source.uid && target.gid == source.gid,
              "entry ownership mismatch: " + path);
        check(target.size == source.size, "entry size mismatch: " + path);
        check(target.content == source.content, "entry content mismatch: " + path);
        check(target.linkTarget == source.linkTarget, "link target mismatch: " + path);
        check(sameTime(target.atime, source.atime), "nanosecond atime mismatch: " + path);
        check(sameTime(target.mtime, source.mtime), "nanosecond mtime mismatch: " + path);
    }
}

uint16_t reservePort() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 && (errno == EPERM || errno == EACCES)) {
        std::cout << "SKIP: TCP sockets are blocked by this sandbox\n";
        return 0;
    }
    check(fd >= 0, "port reservation socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    int bound = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    int error = errno;
    if (bound != 0 && (error == EPERM || error == EACCES)) {
        ::close(fd);
        std::cout << "SKIP: TCP bind is blocked by this sandbox\n";
        return 0;
    }
    check(bound == 0, "port reservation bind failed");
    socklen_t size = sizeof(address);
    check(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0,
          "getsockname failed");
    uint16_t port = ntohs(address.sin_port);
    ::close(fd);
    return port;
}
