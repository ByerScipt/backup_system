#include "helpers.hpp"


void testNetwork(const fs::path& workspace, const fs::path& archive) {
    uint16_t port = reservePort();
    if (port == 0) return;
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
        check(alice.download(id, downloaded.string(), error),
              "network download failed: " + error);
        check(sha256File(downloaded.string()) == sha256File(archive.string()),
              "network roundtrip checksum mismatch");

        fs::path brokenDownload = workspace / "broken-download.bak";
        check(::symlink("missing-download", brokenDownload.c_str()) == 0,
              "broken download symlink fixture failed");
        error.clear();
        check(!alice.download(id, brokenDownload.string(), error),
              "network download overwrote a broken symlink");
        check(fs::is_symlink(brokenDownload),
              "network download did not preserve broken symlink");

        struct stat state{};
        check(::stat((workspace / "server-data" / "users.db").c_str(), &state) == 0 &&
              (state.st_mode & 0777) == 0600, "user database permissions are not 0600");
        fs::path aliceDirectory = workspace / "server-data" / "users" /
            hexDigest(sha256(std::string("alice")));
        check(::stat(aliceDirectory.c_str(), &state) == 0 &&
              (state.st_mode & 0777) == 0700, "user storage directory is not 0700");
        check(::stat((aliceDirectory / (id + ".bak")).c_str(), &state) == 0 &&
              (state.st_mode & 0777) == 0600, "stored archive is not 0600");
        check(::stat((aliceDirectory / (id + ".meta")).c_str(), &state) == 0 &&
              (state.st_mode & 0777) == 0600, "backup metadata is not 0600");

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
            if (event.completed >= 512 * 1024) cancel.store(true);
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

