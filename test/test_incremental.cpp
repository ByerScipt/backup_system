#include "helpers.hpp"
#include "network/internal.hpp"

void testIncremental(uint16_t port, const fs::path& workspace)
{
    namespace protocol = network::detail;
    const auto source = workspace / "incremental-source";
    fs::create_directory(source);
    std::vector<uint8_t> contents(4 * protocol::kChunkSize);
    for (size_t index = 0; index < contents.size(); ++index)
    {
        contents[index] = static_cast<uint8_t>(index / protocol::kChunkSize);
    }
    writeBytes(source / "data", contents);
    const auto original = workspace / "incremental-first.bak";
    check(BackupEngine::create(source.string(), original.string(), {}).success,
          "cannot create incremental fixture");
    network::BackupClient client("127.0.0.1", port, "incremental", "password");
    std::string error, baseId, changedId;
    check(client.registerUser(error), "cannot register incremental account");
    // An empty history falls back to the ordinary upload without a base chain.
    check(client.upload(original.string(), "base", baseId, error, {}, nullptr,
                        true),
          "first incremental upload failed: " + error);
    contents[2 * protocol::kChunkSize + 77] = 99;
    writeBytes(source / "data", contents);
    const auto changed = workspace / "incremental-changed.bak";
    check(BackupEngine::create(source.string(), changed.string(), {}).success,
          "cannot create changed incremental fixture");
    uint64_t previous = 0, reused = 0;
    const auto progress = [&](const ProgressEvent& event)
    {
        if (event.stage == "network-reuse")
        {
            reused += event.completed - previous;
        }
        previous = event.completed;
    };
    check(client.upload(changed.string(), "changed", changedId, error, progress,
                        nullptr, true) &&
              reused >= protocol::kChunkSize,
          "incremental upload did not reuse unchanged blocks: " + error);
    const auto download = workspace / "incremental-download.bak";
    check(client.download(changedId, download.string(), error) &&
              sha256File(download.string()) == sha256File(changed.string()),
          "reconstructed incremental archive checksum differs: " + error);
    check(BackupEngine::restore(download.string(),
                                (workspace / "incremental-out").string(), {})
                  .success &&
              readBytes(workspace /
                        "incremental-out/incremental-source/data") == contents,
          "incremental transfer lost restored file changes");
    std::atomic_bool cancel{false};
    std::string cancelledId;
    check(!client.upload(
              changed.string(), "cancelled", cancelledId, error,
              [&](const ProgressEvent&) { cancel = true; }, &cancel, true) &&
              cancelledId.empty(),
          "cancelled incremental upload reported a successful commit");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(client.list(error).size() == 2 && error.empty(),
          "cancelled incremental upload left a visible backup");

    // Reject references outside the base and malformed reference lengths
    // before publishing anything, even from an authenticated client.
    for (const auto count :
         {0U, 1U, static_cast<unsigned>(protocol::kChunkSize + 1)})
    {
        auto socket = protocol::connectTo("127.0.0.1", port);
        uint32_t request = 1;
        std::vector<uint8_t> login;
        protocol::putString(login, "incremental");
        protocol::sendFrame(socket.get(), protocol::MessageType::LoginStart,
                            request, login);
        auto challenge = protocol::receiveFrame(socket.get());
        check(challenge &&
                  challenge->type == protocol::MessageType::LoginChallenge,
              "incremental malformed-reference login challenge failed");
        protocol::Reader reader(challenge->payload);
        const auto salt = reader.bytes(16), nonce = reader.bytes(16);
        const auto proof =
            protocol::proofFor(protocol::verifierFor("password", salt), nonce);
        protocol::sendFrame(socket.get(), protocol::MessageType::LoginProof,
                            request, {proof.begin(), proof.end()});
        auto authenticated = protocol::receiveFrame(socket.get());
        check(authenticated &&
                  authenticated->type == protocol::MessageType::LoginResponse,
              "incremental malformed-reference login failed");
        std::vector<uint8_t> start;
        protocol::putString(start, "invalid-reference");
        protocol::putU64(start, fs::file_size(original));
        const auto digest = sha256File(original.string());
        start.insert(start.end(), digest.begin(), digest.end());
        protocol::putString(start, baseId);
        protocol::sendFrame(socket.get(),
                            protocol::MessageType::UploadDeltaStart, ++request,
                            start);
        auto frame = protocol::receiveFrame(socket.get());
        check(frame && frame->type == protocol::MessageType::UploadReady,
              "incremental malformed-reference setup failed");
        do
        {
            frame = protocol::receiveFrame(socket.get());
            check(frame &&
                      (frame->type == protocol::MessageType::UploadBaseHashes ||
                       frame->type == protocol::MessageType::UploadBaseEnd),
                  "incremental malformed-reference index failed");
        } while (frame->type != protocol::MessageType::UploadBaseEnd);
        std::vector<uint8_t> reference;
        protocol::putU64(reference, fs::file_size(original));
        protocol::putU32(reference, count);
        protocol::sendFrame(socket.get(), protocol::MessageType::UploadCopy,
                            request, reference);
        frame = protocol::receiveFrame(socket.get());
        check(frame && frame->type == protocol::MessageType::Error,
              "server accepted an invalid incremental reference");
    }
    check(client.list(error).size() == 2 && error.empty(),
          "invalid incremental references published an archive");
    const auto directory =
        protocol::userDirectory(workspace / "server-data", "incremental");
    fs::remove(directory / (baseId + ".bak"));
    fs::remove(directory / (baseId + ".meta"));
    const auto independent = workspace / "incremental-independent.bak";
    check(client.download(changedId, independent.string(), error) &&
              sha256File(independent.string()) == sha256File(changed.string()),
          "incremental archive still depends on its deleted base: " + error);
}
