#include "internal.hpp"

namespace backup::network
{
using namespace detail;

namespace
{
Frame receiveResponse(int fd, MessageType type, uint32_t requestId,
                      std::atomic_bool* cancel)
{
    auto response = receiveFrame(fd, cancel);
    ensure(response.has_value(), "server closed before response");
    if (response->type == MessageType::Error)
    {
        throw NetError(errorFromFrame(*response));
    }
    ensure(response->type == type && response->requestId == requestId,
           "unexpected server response");
    return std::move(*response);
}

void authenticateClient(int fd, uint32_t& requestId,
                        const std::string& username,
                        const std::string& password, std::atomic_bool* cancel)
{
    std::vector<uint8_t> start;
    putString(start, username);
    sendFrame(fd, MessageType::LoginStart, ++requestId, start, cancel);
    auto challenge =
        receiveResponse(fd, MessageType::LoginChallenge, requestId, cancel);
    Reader reader(challenge.payload);
    auto salt = reader.bytes(16);
    auto nonce = reader.bytes(16);
    reader.end();
    auto verifier = verifierFor(password, salt);
    auto proof = proofFor(verifier, nonce);
    std::vector<uint8_t> payload(proof.begin(), proof.end());
    sendFrame(fd, MessageType::LoginProof, requestId, payload, cancel);
    auto response =
        receiveResponse(fd, MessageType::LoginResponse, requestId, cancel);
    Reader status(response.payload);
    ensure(status.u8() == 0, "login rejected");
    status.end();
}

} // namespace

BackupClient::BackupClient(std::string host, uint16_t port,
                           std::string username, std::string password,
                           TlsOptions tls)
    : host_(std::move(host)), port_(port), username_(std::move(username)),
      password_(std::move(password)), tls_(std::move(tls))
{
}

bool BackupClient::registerUser(std::string& error, std::atomic_bool* cancel)
{
    error.clear();
    try
    {
        ensure(validUsername(username_), "invalid username");
        ensure(!password_.empty(), "account password is required");
        Socket socket = connectTo(host_, port_, cancel);
        TlsSession encrypted(socket.get(), clientTlsContext(tls_), false, host_,
                             cancel);
        auto salt = randomBytes(16);
        auto verifier = verifierFor(password_, salt);
        std::vector<uint8_t> payload;
        putString(payload, username_);
        payload.insert(payload.end(), salt.begin(), salt.end());
        payload.insert(payload.end(), verifier.begin(), verifier.end());
        sendFrame(socket.get(), MessageType::RegisterRequest, 1, payload,
                  cancel);
        auto response = receiveResponse(
            socket.get(), MessageType::RegisterResponse, 1, cancel);
        Reader reader(response.payload);
        ensure(reader.u8() == 0, "registration rejected");
        reader.end();
        return true;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
}

bool BackupClient::login(std::string& error, std::atomic_bool* cancel)
{
    error.clear();
    try
    {
        ensure(validUsername(username_), "invalid username");
        ensure(!password_.empty(), "account password is required");
        Socket socket = connectTo(host_, port_, cancel);
        TlsSession encrypted(socket.get(), clientTlsContext(tls_), false, host_,
                             cancel);
        uint32_t request = 0;
        authenticateClient(socket.get(), request, username_, password_, cancel);
        return true;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
}

bool BackupClient::deleteAccount(std::string& error, std::atomic_bool* cancel)
{
    error.clear();
    try
    {
        Socket socket = connectTo(host_, port_, cancel);
        TlsSession encrypted(socket.get(), clientTlsContext(tls_), false, host_,
                             cancel);
        uint32_t request = 0;
        authenticateClient(socket.get(), request, username_, password_, cancel);
        sendFrame(socket.get(), MessageType::DeleteAccountRequest, ++request,
                  {}, cancel);
        auto response = receiveResponse(
            socket.get(), MessageType::DeleteAccountResponse, request, cancel);
        Reader reader(response.payload);
        ensure(reader.u8() == 0, "account deletion rejected");
        reader.end();
        return true;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
}

std::vector<RemoteBackupEntry> BackupClient::list(std::string& error,
                                                  std::atomic_bool* cancel)
{
    error.clear();
    try
    {
        Socket socket = connectTo(host_, port_, cancel);
        TlsSession encrypted(socket.get(), clientTlsContext(tls_), false, host_,
                             cancel);
        uint32_t request = 0;
        authenticateClient(socket.get(), request, username_, password_, cancel);
        sendFrame(socket.get(), MessageType::ListRequest, ++request, {},
                  cancel);
        auto response = receiveResponse(socket.get(), MessageType::ListResponse,
                                        request, cancel);
        Reader reader(response.payload);
        uint32_t count = reader.u32();
        ensure(count <= 4096, "backup list count exceeds limit");
        std::vector<RemoteBackupEntry> entries;
        entries.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            checkCancelled(cancel);
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
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return {};
    }
}

bool BackupClient::upload(const std::string& archivePath,
                          const std::string& displayName, std::string& backupId,
                          std::string& error, ProgressCallback progress,
                          std::atomic_bool* cancel, bool incremental)
{
    error.clear();
    backupId.clear();
    try
    {
        checkCancelled(cancel);
        ensure(displayName.size() <= 256 &&
                   displayName.find('\0') == std::string::npos,
               "invalid backup display name");
        std::string baseId;
        if (incremental)
        {
            const auto previous = list(error, cancel);
            ensure(error.empty(), error);
            if (!previous.empty())
            {
                baseId = previous.front().id;
            }
        }
        const auto size = static_cast<uint64_t>(fs::file_size(archivePath));
        const auto digest = sha256File(archivePath, 0, UINT64_MAX, cancel);
        // Hash before opening a session: large local files must not consume
        // the server's idle timeout or an authenticated connection slot.
        Socket socket = connectTo(host_, port_, cancel);
        TlsSession encrypted(socket.get(), clientTlsContext(tls_), false, host_,
                             cancel);
        uint32_t request = 0;
        authenticateClient(socket.get(), request, username_, password_, cancel);
        checkCancelled(cancel);
        std::vector<uint8_t> start;
        putString(start, displayName);
        putU64(start, size);
        start.insert(start.end(), digest.begin(), digest.end());
        if (!baseId.empty())
        {
            putString(start, baseId);
        }
        sendFrame(socket.get(),
                  baseId.empty() ? MessageType::UploadStart
                                 : MessageType::UploadDeltaStart,
                  ++request, start, cancel);
        auto ready = receiveResponse(socket.get(), MessageType::UploadReady,
                                     request, cancel);
        Reader idReader(ready.payload);
        const std::string preparedId = idReader.string();
        idReader.end();
        ensure(validBackupId(preparedId), "invalid uploaded backup identifier");
        std::vector<std::array<uint8_t, 32>> baseHashes;
        if (!baseId.empty())
        {
            for (;;)
            {
                auto frame = receiveFrame(socket.get(), cancel);
                ensure(frame && frame->requestId == request,
                       "missing incremental block index");
                if (frame->type == MessageType::Error)
                {
                    throw NetError(errorFromFrame(*frame));
                }
                if (frame->type == MessageType::UploadBaseEnd)
                {
                    ensure(frame->payload.empty(), "invalid block index end");
                    break;
                }
                ensure(frame->type == MessageType::UploadBaseHashes &&
                           !frame->payload.empty() &&
                           frame->payload.size() % 32 == 0 &&
                           baseHashes.size() + frame->payload.size() / 32 <=
                               1048576,
                       "invalid incremental block index");
                for (size_t offset = 0; offset < frame->payload.size();
                     offset += 32)
                {
                    std::array<uint8_t, 32> hash{};
                    std::copy_n(frame->payload.begin() + offset, 32,
                                hash.begin());
                    baseHashes.push_back(hash);
                }
            }
        }
        std::ifstream in(archivePath, std::ios::binary);
        ensure(in.good(), "cannot open archive for upload");
        std::vector<uint8_t> chunk(kChunkSize);
        uint64_t sent = 0;
        while (in)
        {
            checkCancelled(cancel);
            in.read(reinterpret_cast<char*>(chunk.data()),
                    static_cast<std::streamsize>(chunk.size()));
            size_t got = static_cast<size_t>(in.gcount());
            if (got == 0)
            {
                continue;
            }
            chunk.resize(got);
            const auto index = sent / kChunkSize;
            const bool reused =
                index < baseHashes.size() && sha256(chunk) == baseHashes[index];
            if (reused)
            {
                std::vector<uint8_t> reference;
                putU64(reference, sent);
                putU32(reference, static_cast<uint32_t>(got));
                sendFrame(socket.get(), MessageType::UploadCopy, request,
                          reference, cancel);
            }
            else
            {
                sendFrame(socket.get(), MessageType::UploadChunk, request,
                          chunk, cancel);
            }
            chunk.resize(kChunkSize);
            sent += got;
            reportProgress(progress,
                           reused ? "network-reuse" : "network-upload", sent,
                           size);
        }
        ensure(!in.bad() && sent == size,
               "archive changed or could not be read during upload");
        checkCancelled(cancel);
        sendFrame(socket.get(), MessageType::UploadEnd, request, {}, cancel);
        auto result = receiveResponse(socket.get(), MessageType::UploadResult,
                                      request, cancel);
        Reader done(result.payload);
        ensure(done.string() == preparedId,
               "uploaded backup identifier changed");
        done.end();
        backupId = preparedId;
        return true;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
}

bool BackupClient::download(const std::string& backupId,
                            const std::string& outputPath, std::string& error,
                            ProgressCallback progress, std::atomic_bool* cancel)
{
    error.clear();
    try
    {
        checkCancelled(cancel);
        ensure(validBackupId(backupId), "invalid backup identifier");
        struct stat outputState
        {
        };
        ensure(::lstat(outputPath.c_str(), &outputState) != 0,
               "download output already exists");
        ensure(errno == ENOENT, "cannot inspect download output path");
        Socket socket = connectTo(host_, port_, cancel);
        TlsSession encrypted(socket.get(), clientTlsContext(tls_), false, host_,
                             cancel);
        uint32_t request = 0;
        authenticateClient(socket.get(), request, username_, password_, cancel);
        checkCancelled(cancel);
        std::vector<uint8_t> payload;
        putString(payload, backupId);
        sendFrame(socket.get(), MessageType::DownloadRequest, ++request,
                  payload, cancel);
        auto start = receiveResponse(socket.get(), MessageType::DownloadStart,
                                     request, cancel);
        Reader reader(start.payload);
        uint64_t declaredSize = reader.u64();
        auto digestBytes = reader.bytes(32);
        reader.end();
        std::array<uint8_t, 32> declaredDigest{};
        std::copy(digestBytes.begin(), digestBytes.end(),
                  declaredDigest.begin());
        fs::path output = fs::absolute(outputPath);
        fs::create_directories(output.parent_path());
        backup::detail::TempFile staging(output.parent_path());
        const auto& temp = staging.path();
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        ensure(out.good(), "cannot create download output");
        uint64_t received = 0;
        while (true)
        {
            checkCancelled(cancel);
            auto frame = receiveFrame(socket.get(), cancel);
            ensure(frame.has_value(), "server disconnected during download");
            ensure(frame->requestId == request,
                   "download request identifier changed");
            if (frame->type == MessageType::DownloadEnd)
            {
                ensure(frame->payload.empty(),
                       "download end has trailing data");
                break;
            }
            ensure(frame->type == MessageType::DownloadChunk,
                   "unexpected download message");
            ensure(frame->payload.size() <= declaredSize - received,
                   "download exceeds declared size");
            if (!frame->payload.empty())
            {
                out.write(reinterpret_cast<const char*>(frame->payload.data()),
                          static_cast<std::streamsize>(frame->payload.size()));
            }
            ensure(out.good(), "cannot write download output");
            received += frame->payload.size();
            reportProgress(progress, "network-download", received,
                           declaredSize);
        }
        checkCancelled(cancel);
        out.close();
        ensure(out.good(), "cannot close download output");
        ensure(received == declaredSize, "downloaded size mismatch");
        ensure(sha256File(temp.string(), 0, UINT64_MAX, cancel) ==
                   declaredDigest,
               "downloaded SHA-256 mismatch");
        checkCancelled(cancel);
        commitFileNoReplace(temp, output);
        return true;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
}

} // namespace backup::network
