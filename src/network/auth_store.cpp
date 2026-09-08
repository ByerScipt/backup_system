#include "internal.hpp"

namespace backup {
namespace network {
namespace detail {

std::vector<uint8_t> randomBytes(size_t count) {
    std::random_device random;
    std::vector<uint8_t> out(count);
    for (auto& b : out)
        b = static_cast<uint8_t>(random());
    return out;
}

std::array<uint8_t, 32> verifierFor(const std::string& password, const std::vector<uint8_t>& salt) {
    std::vector<uint8_t> material(password.begin(), password.end());
    material.insert(material.end(), salt.begin(), salt.end());
    auto digest = sha256(material);
    for (int i = 0; i < 4096; ++i) {
        std::vector<uint8_t> next(digest.begin(), digest.end());
        next.insert(next.end(), salt.begin(), salt.end());
        digest = sha256(next);
    }
    return digest;
}

std::array<uint8_t, 32> proofFor(const std::array<uint8_t, 32>& verifier,
                                 const std::vector<uint8_t>& nonce) {
    std::vector<uint8_t> data(verifier.begin(), verifier.end());
    data.insert(data.end(), nonce.begin(), nonce.end());
    return sha256(data);
}

bool validUsername(const std::string& value) {
    if (value.empty() || value.size() > 64)
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.';
    });
}

bool validBackupId(const std::string& value) {
    return value.size() == 32 && std::all_of(value.begin(), value.end(),
                                             [](unsigned char c) { return std::isxdigit(c); });
}

std::string randomId() {
    auto bytes = randomBytes(16);
    std::ostringstream out;
    out << std::hex;
    for (uint8_t b : bytes) {
        out.width(2);
        out.fill('0');
        out << static_cast<unsigned>(b);
    }
    return out.str();
}

std::mutex gStorageMutex;

std::vector<UserRecord> loadUsers(const fs::path& storage) {
    std::vector<UserRecord> users;
    fs::path path = storage / "users.db";
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return users;
    std::array<char, 4> magic{};
    in.read(magic.data(), 4);
    ensure(in.good() && magic == std::array<char, 4>{{'U', 'S', 'R', '1'}},
           "invalid user database");
    uint8_t b[4]{};
    in.read(reinterpret_cast<char*>(b), 4);
    ensure(in.good(), "truncated user database");
    uint32_t count = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
                     (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    ensure(count <= 100000, "user database count exceeds limit");
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t nbuf[2]{};
        in.read(reinterpret_cast<char*>(nbuf), 2);
        ensure(in.good(), "truncated user database");
        uint16_t n = nbuf[0] | (nbuf[1] << 8);
        UserRecord u;
        u.name.resize(n);
        in.read(u.name.data(), n);
        in.read(reinterpret_cast<char*>(u.salt.data()), u.salt.size());
        in.read(reinterpret_cast<char*>(u.verifier.data()), u.verifier.size());
        ensure(in.good() && validUsername(u.name), "invalid user database record");
        users.push_back(u);
    }
    return users;
}

void saveUsers(const fs::path& storage, const std::vector<UserRecord>& users) {
    fs::create_directories(storage);
    fs::path temp = storage / (".users-" + randomId() + ".tmp");
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    ensure(out.good(), "cannot write user database");
    ensure(chmod(temp.c_str(), 0600) == 0, "cannot secure user database permissions");
    out.write("USR1", 4);
    uint32_t count = static_cast<uint32_t>(users.size());
    uint8_t cb[4]{static_cast<uint8_t>(count), static_cast<uint8_t>(count >> 8),
                  static_cast<uint8_t>(count >> 16), static_cast<uint8_t>(count >> 24)};
    out.write(reinterpret_cast<char*>(cb), 4);
    for (const auto& u : users) {
        uint16_t n = static_cast<uint16_t>(u.name.size());
        uint8_t nb[2]{static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8)};
        out.write(reinterpret_cast<char*>(nb), 2);
        out.write(u.name.data(), n);
        out.write(reinterpret_cast<const char*>(u.salt.data()), u.salt.size());
        out.write(reinterpret_cast<const char*>(u.verifier.data()), u.verifier.size());
    }
    out.close();
    ensure(out.good(), "cannot finalize user database");
    std::error_code ec;
    fs::rename(temp, storage / "users.db", ec);
    if (ec)
        fs::remove(temp);
    ensure(!ec, "cannot commit user database");
}

fs::path userDirectory(const fs::path& storage, const std::string& username) {
    return storage / "users" / hexDigest(sha256(username));
}

void writeMeta(const fs::path& path, const RemoteBackupEntry& entry) {
    fs::path temp = path.string() + ".tmp-" + randomId();
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    ensure(out.good(), "cannot write backup metadata");
    ensure(chmod(temp.c_str(), 0600) == 0, "cannot secure backup metadata permissions");
    out.write("MET1", 4);
    uint16_t n = static_cast<uint16_t>(entry.name.size());
    uint8_t nb[2]{static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8)};
    out.write(reinterpret_cast<char*>(nb), 2);
    out.write(entry.name.data(), n);
    auto little64 = [&](uint64_t v) {
        uint8_t b[8];
        for (int i = 0; i < 8; ++i)
            b[i] = static_cast<uint8_t>(v >> (i * 8));
        out.write(reinterpret_cast<char*>(b), 8);
    };
    little64(entry.timestamp);
    little64(entry.size);
    out.write(entry.digest.data(), static_cast<std::streamsize>(entry.digest.size()));
    out.close();
    ensure(out.good(), "cannot finalize backup metadata");
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec)
        fs::remove(temp);
    ensure(!ec, "cannot commit backup metadata");
}

RemoteBackupEntry readMeta(const fs::path& path, const std::string& id) {
    std::ifstream in(path, std::ios::binary);
    ensure(in.good(), "cannot read backup metadata");
    std::array<char, 4> magic{};
    in.read(magic.data(), 4);
    ensure(in.good() && magic == std::array<char, 4>{{'M', 'E', 'T', '1'}},
           "invalid backup metadata");
    uint8_t nb[2]{};
    in.read(reinterpret_cast<char*>(nb), 2);
    uint16_t n = nb[0] | (nb[1] << 8);
    ensure(n <= 4096, "backup name is too long");
    RemoteBackupEntry e;
    e.id = id;
    e.name.resize(n);
    in.read(e.name.data(), n);
    auto little64 = [&]() {
        uint8_t b[8]{};
        in.read(reinterpret_cast<char*>(b), 8);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(b[i]) << (i * 8);
        return v;
    };
    e.timestamp = little64();
    e.size = little64();
    e.digest.resize(64);
    in.read(e.digest.data(), 64);
    ensure(in.good(), "truncated backup metadata");
    return e;
}

std::vector<RemoteBackupEntry> listEntries(const fs::path& storage, const std::string& username) {
    std::vector<RemoteBackupEntry> entries;
    fs::path dir = userDirectory(storage, username);
    std::error_code ec;
    if (!fs::exists(dir, ec))
        return entries;
    for (const auto& item : fs::directory_iterator(dir, ec)) {
        ensure(!ec, "cannot enumerate user backups");
        std::string name = item.path().filename().string();
        if (name.size() == 37 && name.substr(32) == ".meta") {
            std::string id = name.substr(0, 32);
            if (validBackupId(id))
                entries.push_back(readMeta(item.path(), id));
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.timestamp > b.timestamp; });
    return entries;
}

std::optional<UserRecord> findUser(const fs::path& storage, const std::string& name) {
    std::lock_guard<std::mutex> lock(gStorageMutex);
    for (const auto& user : loadUsers(storage))
        if (user.name == name)
            return user;
    return std::nullopt;
}

void setTimeouts(int fd, uint32_t seconds) {
    timeval timeout{static_cast<time_t>(seconds), 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

} // namespace detail
} // namespace network
} // namespace backup
