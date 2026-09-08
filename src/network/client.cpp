#include "internal.hpp"

namespace backup::network {
using namespace detail;

namespace {
bool authenticateClient(int fd, uint32_t& requestId, const std::string& username,
                        const std::string& password, std::string& error) {
    try {
        std::vector<uint8_t> start;
        putString(start, username);
        sendFrame(fd, MessageType::LoginStart, ++requestId, start);
        auto challenge = receiveFrame(fd);
        ensure(challenge.has_value(), "server closed during login");
        if (challenge->type == MessageType::Error)
            throw NetError(errorFromFrame(*challenge));
        ensure(challenge->type == MessageType::LoginChallenge && challenge->requestId == requestId,
               "unexpected login challenge");
        Reader reader(challenge->payload);
        auto salt = reader.bytes(16);
        auto nonce = reader.bytes(16);
        reader.end();
        auto verifier = verifierFor(password, salt);
        auto proof = proofFor(verifier, nonce);
        std::vector<uint8_t> payload(proof.begin(), proof.end());
        sendFrame(fd, MessageType::LoginProof, requestId, payload);
        auto response = receiveFrame(fd);
        ensure(response.has_value(), "server closed during login");
        if (response->type == MessageType::Error)
            throw NetError(errorFromFrame(*response));
        ensure(response->type == MessageType::LoginResponse && response->requestId == requestId,
               "unexpected login response");
        Reader status(response->payload);
        ensure(status.u8() == 0, "login rejected");
        status.end();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace

BackupClient::BackupClient(std::string host, uint16_t port, std::string username,
                           std::string password)
    : host_(std::move(host)), port_(port), username_(std::move(username)),
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
        if (response->type == MessageType::Error)
            throw NetError(errorFromFrame(*response));
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
        if (response->type == MessageType::Error)
            throw NetError(errorFromFrame(*response));
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

bool BackupClient::upload(const std::string& archivePath, const std::string& displayName,
                          std::string& backupId, std::string& error, ProgressCallback progress,
                          std::atomic_bool* cancel) {
    try {
        checkCancelled(cancel);
        Socket socket = connectTo(host_, port_);
        uint32_t request = 0;
        ensure(authenticateClient(socket.get(), request, username_, password_, error), error);
        checkCancelled(cancel);
        uint64_t size = static_cast<uint64_t>(fs::file_size(archivePath));
        auto digest = sha256File(archivePath);
        checkCancelled(cancel);
        std::vector<uint8_t> start;
        putString(start, displayName);
        putU64(start, size);
        start.insert(start.end(), digest.begin(), digest.end());
        sendFrame(socket.get(), MessageType::UploadStart, ++request, start);
        auto ready = receiveFrame(socket.get());
        ensure(ready.has_value(), "server closed before upload");
        if (ready->type == MessageType::Error)
            throw NetError(errorFromFrame(*ready));
        ensure(ready->type == MessageType::UploadReady && ready->requestId == request,
               "unexpected upload response");
        Reader idReader(ready->payload);
        backupId = idReader.string();
        idReader.end();
        std::ifstream in(archivePath, std::ios::binary);
        ensure(in.good(), "cannot open archive for upload");
        std::vector<uint8_t> chunk(kChunkSize);
        uint64_t sent = 0;
        while (in) {
            checkCancelled(cancel);
            in.read(reinterpret_cast<char*>(chunk.data()),
                    static_cast<std::streamsize>(chunk.size()));
            size_t got = static_cast<size_t>(in.gcount());
            if (got == 0)
                continue;
            sendFrame(socket.get(), MessageType::UploadChunk, request,
                      {chunk.begin(), chunk.begin() + static_cast<ptrdiff_t>(got)});
            sent += got;
            reportProgress(progress, "network-upload", sent, size);
        }
        checkCancelled(cancel);
        sendFrame(socket.get(), MessageType::UploadEnd, request);
        auto result = receiveFrame(socket.get());
        ensure(result.has_value(), "server closed after upload");
        if (result->type == MessageType::Error)
            throw NetError(errorFromFrame(*result));
        ensure(result->type == MessageType::UploadResult, "unexpected upload completion response");
        Reader done(result->payload);
        backupId = done.string();
        done.end();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool BackupClient::download(const std::string& backupId, const std::string& outputPath,
                            std::string& error, ProgressCallback progress,
                            std::atomic_bool* cancel) {
    try {
        checkCancelled(cancel);
        ensure(validBackupId(backupId), "invalid backup identifier");
        struct stat outputState {};
        ensure(::lstat(outputPath.c_str(), &outputState) != 0, "download output already exists");
        ensure(errno == ENOENT, "cannot inspect download output path");
        Socket socket = connectTo(host_, port_);
        uint32_t request = 0;
        ensure(authenticateClient(socket.get(), request, username_, password_, error), error);
        checkCancelled(cancel);
        std::vector<uint8_t> payload;
        putString(payload, backupId);
        sendFrame(socket.get(), MessageType::DownloadRequest, ++request, payload);
        auto start = receiveFrame(socket.get());
        ensure(start.has_value(), "server closed before download");
        if (start->type == MessageType::Error)
            throw NetError(errorFromFrame(*start));
        ensure(start->type == MessageType::DownloadStart && start->requestId == request,
               "unexpected download response");
        Reader reader(start->payload);
        uint64_t declaredSize = reader.u64();
        auto digestBytes = reader.bytes(32);
        reader.end();
        std::array<uint8_t, 32> declaredDigest{};
        std::copy(digestBytes.begin(), digestBytes.end(), declaredDigest.begin());
        fs::path output = fs::absolute(outputPath);
        fs::create_directories(output.parent_path());
        fs::path temp = output.string() + ".tmp-" + randomId();
        struct Cleanup {
            fs::path path;
            ~Cleanup() {
                std::error_code ec;
                fs::remove(path, ec);
            }
        } cleanup{temp};
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        ensure(out.good(), "cannot create download output");
        ::chmod(temp.c_str(), 0600);
        uint64_t received = 0;
        while (true) {
            checkCancelled(cancel);
            auto frame = receiveFrame(socket.get());
            ensure(frame.has_value(), "server disconnected during download");
            ensure(frame->requestId == request, "download request identifier changed");
            if (frame->type == MessageType::DownloadEnd)
                break;
            ensure(frame->type == MessageType::DownloadChunk, "unexpected download message");
            ensure(frame->payload.size() <= declaredSize - received,
                   "download exceeds declared size");
            if (!frame->payload.empty()) {
                out.write(reinterpret_cast<const char*>(frame->payload.data()),
                          static_cast<std::streamsize>(frame->payload.size()));
            }
            ensure(out.good(), "cannot write download output");
            received += frame->payload.size();
            reportProgress(progress, "network-download", received, declaredSize);
        }
        checkCancelled(cancel);
        out.close();
        ensure(received == declaredSize, "downloaded size mismatch");
        ensure(sha256File(temp.string()) == declaredDigest, "downloaded SHA-256 mismatch");
        commitFileNoReplace(temp, output);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace backup::network
