#ifndef BACKUP_NETWORK_HPP
#define BACKUP_NETWORK_HPP

// Account-isolated archive transfers over NBKP/TCP (no TLS). Archive passwords
// belong to backup_core and are independent of these account credentials.

#include "core.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace backup::network
{

// True when value is an IPv4 address literal, the only form the listener
// accepts; host names are rejected so startup never depends on DNS.
bool isIPv4Address(const std::string& value);

struct ServerConfig
{
    // "0.0.0.0" accepts every interface. Bind "127.0.0.1" when the server is
    // meant for the local machine only; NBKP itself has no TLS.
    std::string bindAddress = "0.0.0.0";
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
    // A bindAddress that is not an IPv4 literal fails run() without binding.
    // Keep this object alive until run() has returned and its thread is joined.
    bool run();
    // Requests cancellation; run() owns and closes its listener. Idle accept
    // waits poll this flag at most every 100 ms, then join active sessions.
    void stop();

private:
    ServerConfig config_;
    std::atomic_bool stopRequested_{false};
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
    // Verifies credentials; each transfer still authenticates independently.
    bool login(std::string& error, std::atomic_bool* cancel = nullptr);
    // Deletes only this authenticated account. Refuses stored backups and
    // other active sessions. A lost acknowledgement cannot undo the deletion.
    bool deleteAccount(std::string& error, std::atomic_bool* cancel = nullptr);
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
