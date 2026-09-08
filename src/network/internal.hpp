#pragma once
// Internal header for the network service. Public API stays in
// include/backup/network.hpp; everything here is backup::network::detail.
#include "backup/network.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace backup {
namespace network {
namespace detail {

inline constexpr uint16_t kProtocolVersion = 1;
inline constexpr uint32_t kMaxPayload = 1024 * 1024;
inline constexpr size_t kChunkSize = 512 * 1024;
inline constexpr std::array<char, 4> kMagic{{'N', 'B', 'K', 'P'}};

enum class MessageType : uint16_t {
    RegisterRequest = 1,
    RegisterResponse = 2,
    LoginStart = 3,
    LoginChallenge = 4,
    LoginProof = 5,
    LoginResponse = 6,
    UploadStart = 10,
    UploadReady = 11,
    UploadChunk = 12,
    UploadEnd = 13,
    UploadResult = 14,
    ListRequest = 20,
    ListResponse = 21,
    DownloadRequest = 30,
    DownloadStart = 31,
    DownloadChunk = 32,
    DownloadEnd = 33,
    Error = 255
};

struct NetError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void ensure(bool condition, const std::string& message);
void checkCancelled(std::atomic_bool* cancel);
void reportProgress(const ProgressCallback& progress, const std::string& stage, uint64_t completed,
                    uint64_t total);

class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() {
        close();
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    int get() const {
        return fd_;
    }
    bool valid() const {
        return fd_ >= 0;
    }
    int release() {
        int value = fd_;
        fd_ = -1;
        return value;
    }
    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

void sendAll(int fd, const uint8_t* data, size_t size);
bool receiveAll(int fd, uint8_t* data, size_t size, bool allowCleanEof = false);
void putU16(std::vector<uint8_t>& out, uint16_t value);
void putU32(std::vector<uint8_t>& out, uint32_t value);
void putU64(std::vector<uint8_t>& out, uint64_t value);
void putString(std::vector<uint8_t>& out, const std::string& value);

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& data) : data_(data) {}
    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    std::string string();
    std::vector<uint8_t> bytes(size_t n);
    void end() const;

private:
    const std::vector<uint8_t>& data_;
    size_t offset_ = 0;
    void need(size_t n) const;
};

struct Frame {
    MessageType type = MessageType::Error;
    uint32_t requestId = 0;
    std::vector<uint8_t> payload;
};

void sendFrame(int fd, MessageType type, uint32_t requestId,
               const std::vector<uint8_t>& payload = {});
std::optional<Frame> receiveFrame(int fd);
void sendError(int fd, uint32_t requestId, const std::string& message);
std::string errorFromFrame(const Frame& frame);
Socket connectTo(const std::string& host, uint16_t port);

// auth helpers
std::vector<uint8_t> randomBytes(size_t count);
std::array<uint8_t, 32> verifierFor(const std::string& password, const std::vector<uint8_t>& salt);
std::array<uint8_t, 32> proofFor(const std::array<uint8_t, 32>& verifier,
                                 const std::vector<uint8_t>& nonce);
bool validUsername(const std::string& value);
bool validBackupId(const std::string& value);
std::string randomId();
void commitFileNoReplace(const fs::path& temporary, const fs::path& output);

struct UserRecord {
    std::string name;
    std::array<uint8_t, 16> salt{};
    std::array<uint8_t, 32> verifier{};
};
extern std::mutex gStorageMutex;
std::vector<UserRecord> loadUsers(const fs::path& storage);
void saveUsers(const fs::path& storage, const std::vector<UserRecord>& users);
fs::path userDirectory(const fs::path& storage, const std::string& username);
void writeMeta(const fs::path& path, const RemoteBackupEntry& entry);
RemoteBackupEntry readMeta(const fs::path& path, const std::string& id);
std::vector<RemoteBackupEntry> listEntries(const fs::path& storage, const std::string& username);
std::optional<UserRecord> findUser(const fs::path& storage, const std::string& name);
void setTimeouts(int fd, uint32_t seconds);
void handleSession(int fd, ServerConfig config);

} // namespace detail
} // namespace network
} // namespace backup
