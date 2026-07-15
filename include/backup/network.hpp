#ifndef BACKUP_NETWORK_HPP
#define BACKUP_NETWORK_HPP

#include "backup/core.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace backup::network {

struct ServerConfig {
    uint16_t port = 8848;
    std::string storagePath = "./server_data";
    uint32_t maxConnections = 32;
    uint32_t timeoutSeconds = 30;

    static ServerConfig load(const std::string& path);
};

struct RemoteBackupEntry {
    std::string id;
    std::string name;
    uint64_t timestamp = 0;
    uint64_t size = 0;
    std::string digest;
};

class BackupServer {
public:
    explicit BackupServer(ServerConfig config);
    bool run();
    void stop();

private:
    ServerConfig config_;
    std::atomic_bool running_{false};
    int listenFd_ = -1;
};

class BackupClient {
public:
    BackupClient(std::string host, uint16_t port,
                 std::string username, std::string password);

    bool registerUser(std::string& error);
    bool upload(const std::string& archivePath, const std::string& displayName,
                std::string& backupId, std::string& error,
                ProgressCallback progress = {}, std::atomic_bool* cancel = nullptr);
    std::vector<RemoteBackupEntry> list(std::string& error);
    bool download(const std::string& backupId, const std::string& outputPath,
                  std::string& error, ProgressCallback progress = {},
                  std::atomic_bool* cancel = nullptr);

private:
    std::string host_;
    uint16_t port_;
    std::string username_;
    std::string password_;
};

} // namespace backup::network

#endif
