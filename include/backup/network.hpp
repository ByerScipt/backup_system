#ifndef BACKUP_NETWORK_HPP
#define BACKUP_NETWORK_HPP

// Account-isolated archive transfers over NBKP/TCP (no TLS). Archive passwords
// belong to backup_core and are independent of these account credentials.

#include "backup/core.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace backup::network
{

struct ServerConfig
{
    uint16_t port = 8848;
    std::string storagePath = "./server_data";
    uint32_t maxConnections = 32;
    uint32_t timeoutSeconds = 30;

    static ServerConfig load(const std::string& path);
};

struct RemoteBackupEntry
{
    std::string id;
    std::string name;
    uint64_t timestamp = 0;
    uint64_t size = 0;
    std::string digest;
};

class BackupServer
{
public:
    explicit BackupServer(ServerConfig config);
    // Blocks until stop() or a startup failure; run on a dedicated thread.
    // Keep this object alive until run() has returned and its thread is joined.
    bool run();
    void stop();

private:
    ServerConfig config_;
    std::atomic_bool running_{false};
    std::atomic_int listenFd_{-1};
};

class BackupClient
{
public:
    BackupClient(std::string host, uint16_t port, std::string username,
                 std::string password);

    // Synchronous calls: false/non-empty error indicates failure. On list(),
    // empty results mean "no backups" only if error is empty. Progress runs on
    // the caller's thread; cancel must live until the call returns. Socket
    // connect/read/write waits time out after 30 seconds and poll cancellation.
    // DNS resolution remains subject to the system resolver's timeout.
    bool registerUser(std::string& error, std::atomic_bool* cancel = nullptr);
    // Clears backupId on entry and sets it only after a validated
    // acknowledgement. A lost/cancelled acknowledgement cannot undo a
    // server-side commit.
    bool upload(const std::string& archivePath, const std::string& displayName,
                std::string& backupId, std::string& error,
                ProgressCallback progress = {},
                std::atomic_bool* cancel = nullptr);
    std::vector<RemoteBackupEntry> list(std::string& error,
                                        std::atomic_bool* cancel = nullptr);
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
