#include "helpers.hpp"
#include "network/internal.hpp"

namespace {
void testServerConfig(const fs::path& workspace) {
    const fs::path path = workspace / "server.conf";
    for (const std::string setting : {"port = 65537", "port = -65535", "port = 8848junk",
                                      "max_connections = 4294967297", "timeout_seconds = -1"}) {
        std::ofstream(path) << setting << '\n';
        bool rejected = false;
        try {
            network::ServerConfig::load(path.string());
        } catch (const std::exception&) {
            rejected = true;
        }
        check(rejected, "invalid server configuration was accepted: " + setting);
    }
    std::ofstream(path) << "port = 65535\nmax_connections = 1\ntimeout_seconds = 3\n";
    auto config = network::ServerConfig::load(path.string());
    check(config.port == 65535 && config.maxConnections == 1 && config.timeoutSeconds == 3,
          "valid server configuration changed");
}

void testLoginRequestId(uint16_t port) {
    namespace protocol = network::detail;
    auto socket = protocol::connectTo("127.0.0.1", port);
    std::vector<uint8_t> payload;
    protocol::putString(payload, "alice");
    protocol::sendFrame(socket.get(), protocol::MessageType::LoginStart, 10, payload);
    auto challenge = protocol::receiveFrame(socket.get());
    check(challenge && challenge->type == protocol::MessageType::LoginChallenge,
          "login challenge was not received");
    protocol::Reader reader(challenge->payload);
    auto salt = reader.bytes(16);
    auto nonce = reader.bytes(16);
    auto proof = protocol::proofFor(protocol::verifierFor("alice-password", salt), nonce);
    protocol::sendFrame(socket.get(), protocol::MessageType::LoginProof, 11,
                        {proof.begin(), proof.end()});
    auto response = protocol::receiveFrame(socket.get());
    check(response && response->type == protocol::MessageType::Error,
          "login proof with a mismatched request ID was accepted");
}
} // namespace

void testNetwork(const fs::path& workspace, const fs::path& archive) {
    testServerConfig(workspace);
    uint16_t port = reservePort();
    if (port == 0)
        return;
    network::ServerConfig config;
    config.port = port;
    config.storagePath = (workspace / "server-data").string();
    config.timeoutSeconds = 3;
    network::BackupServer server(config);
    std::thread serverThread([&] { check(server.run(), "server run failed"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    try {
        std::string error;
        network::BackupClient alice("127.0.0.1", port, "alice", "alice-password");
        check(alice.registerUser(error), "Alice registration failed: " + error);
        testLoginRequestId(port);
        error.clear();
        check(!alice.registerUser(error), "duplicate registration was accepted");
        error.clear();

        std::string id;
        check(alice.upload(archive.string(), "integration backup", id, error),
              "network upload failed: " + error);
        check(id.size() == 32, "server returned invalid backup ID");
        error.clear();
        auto entries = alice.list(error);
        check(error.empty() && entries.size() == 1 && entries[0].id == id,
              "network list mismatch: " + error);
        fs::path downloaded = workspace / "downloaded.bak";
        check(alice.download(id, downloaded.string(), error), "network download failed: " + error);
        check(sha256File(downloaded.string()) == sha256File(archive.string()),
              "network roundtrip checksum mismatch");

        fs::path brokenDownload = workspace / "broken-download.bak";
        check(::symlink("missing-download", brokenDownload.c_str()) == 0,
              "broken download symlink fixture failed");
        error.clear();
        check(!alice.download(id, brokenDownload.string(), error),
              "network download overwrote a broken symlink");
        check(fs::is_symlink(brokenDownload), "network download did not preserve broken symlink");

        struct stat state {};
        check(::stat((workspace / "server-data" / "users.db").c_str(), &state) == 0 &&
                  (state.st_mode & 0777) == 0600,
              "user database permissions are not 0600");
        fs::path aliceDirectory =
            workspace / "server-data" / "users" / hexDigest(sha256(std::string("alice")));
        check(::stat(aliceDirectory.c_str(), &state) == 0 && (state.st_mode & 0777) == 0700,
              "user storage directory is not 0700");
        check(::stat((aliceDirectory / (id + ".bak")).c_str(), &state) == 0 &&
                  (state.st_mode & 0777) == 0600,
              "stored archive is not 0600");
        check(::stat((aliceDirectory / (id + ".meta")).c_str(), &state) == 0 &&
                  (state.st_mode & 0777) == 0600,
              "backup metadata is not 0600");

        network::BackupClient wrong("127.0.0.1", port, "alice", "wrong");
        error.clear();
        auto denied = wrong.list(error);
        check(denied.empty() && !error.empty(), "wrong account password was accepted");
        network::BackupClient bob("127.0.0.1", port, "bob", "bob-password");
        error.clear();
        check(bob.registerUser(error), "Bob registration failed: " + error);
        error.clear();
        check(bob.list(error).empty() && error.empty(), "cross-user list isolation failed");
        error.clear();
        check(!bob.download(id, (workspace / "bob-download.bak").string(), error),
              "cross-user download was accepted");

        fs::path cancelledUpload = workspace / "cancelled-upload.bin";
        writeBytes(cancelledUpload, std::vector<uint8_t>(2 * 1024 * 1024, 0x5a));
        std::atomic_bool cancel{false};
        std::string cancelledId;
        auto cancelAfterChunk = [&](const ProgressEvent& event) {
            if (event.completed >= 512 * 1024)
                cancel.store(true);
        };
        error.clear();
        check(!alice.upload(cancelledUpload.string(), "cancelled", cancelledId, error,
                            cancelAfterChunk, &cancel),
              "cancelled upload unexpectedly completed");
        check(error.find("cancelled") != std::string::npos,
              "cancelled upload returned the wrong error");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        error.clear();
        entries = alice.list(error);
        check(error.empty() && entries.size() == 1,
              "cancelled upload left a visible half-finished backup");

        for (int attempt = 0; attempt < 100; ++attempt) {
            error.clear();
            entries = alice.list(error);
            check(error.empty() && entries.size() == 1,
                  "server failed while recycling completed sessions");
        }
    } catch (...) {
        server.stop();
        serverThread.join();
        throw;
    }
    server.stop();
    serverThread.join();

    network::BackupServer restarted(config);
    std::thread restartThread([&] { check(restarted.run(), "restarted server run failed"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    try {
        std::string error;
        network::BackupClient alice("127.0.0.1", port, "alice", "alice-password");
        auto entries = alice.list(error);
        check(error.empty() && entries.size() == 1,
              "server restart did not preserve backup history: " + error);
    } catch (...) {
        restarted.stop();
        restartThread.join();
        throw;
    }
    restarted.stop();
    restartThread.join();
}
