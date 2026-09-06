#include "net_internal.hpp"

namespace backup {
namespace network {
using namespace detail;

BackupClient::BackupClient(std::string host, uint16_t port, std::string username,
                           std::string password)
    : host_(std::move(host)),
      port_(port),
      username_(std::move(username)),
      password_(std::move(password)) {}

bool BackupClient::registerUser(std::string& error) {
    try {
        ensure(validUsername(username_), "invalid username");
        ensure(!password_.empty(), "account password is required");
        Socket socket = connectTo(host_, port_);
        auto salt = randomBytes(16);
        auto verifier = verifierFor(password_, salt);
        std::vector<uint8_t> payload;
        putString(payload, username_);
        payload.insert(payload.end(), salt.begin(), salt.end());
        payload.insert(payload.end(), verifier.begin(), verifier.end());
        sendFrame(socket.get(), MessageType::RegisterRequest, 1, payload);
        auto response = receiveFrame(socket.get());
        ensure(response.has_value(), "server closed during registration");
        if (response->type == MessageType::Error) throw NetError(errorFromFrame(*response));
        ensure(response->type == MessageType::RegisterResponse, "unexpected registration response");
        Reader reader(response->payload);
        ensure(reader.u8() == 0, "registration rejected");
        reader.end();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

std::vector<RemoteBackupEntry> BackupClient::list(std::string& error) {
    try {
        Socket socket = connectTo(host_, port_);
        uint32_t request = 0;
        ensure(authenticateClient(socket.get(), request, username_, password_, error), error);
        sendFrame(socket.get(), MessageType::ListRequest, ++request);
        auto response = receiveFrame(socket.get());
        ensure(response.has_value(), "server closed during list request");
        if (response->type == MessageType::Error) throw NetError(errorFromFrame(*response));
        ensure(response->type == MessageType::ListResponse, "unexpected list response");
        Reader reader(response->payload);
        uint32_t count = reader.u32();
        ensure(count <= 4096, "backup list count exceeds limit");
        std::vector<RemoteBackupEntry> entries;
        entries.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            RemoteBackupEntry entry;
            entry.id = reader.string();
            entry.name = reader.string();
            entry.timestamp = reader.u64();
            entry.size = reader.u64();
            entry.digest = reader.string();
            entries.push_back(std::move(entry));
        }
        reader.end();
        return entries;
    } catch (const std::exception& e) {
        error = e.what();
        return {};
    }
}

}  // namespace network
}  // namespace backup
