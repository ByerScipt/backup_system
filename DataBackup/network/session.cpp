#include "../file_io.hpp"
#include "internal.hpp"

namespace backup::network::detail
{

namespace
{

using SessionKey = std::pair<fs::path, std::string>;
// Guarded by gStorageMutex, together with the account database. No account
// may disappear while another authenticated connection can write its files.
std::map<SessionKey, size_t> activeSessions;

class AuthenticatedSession
{
public:
    ~AuthenticatedSession()
    {
        if (key_)
        {
            std::lock_guard<std::mutex> lock(gStorageMutex);
            auto found = activeSessions.find(*key_);
            if (--found->second == 0)
            {
                activeSessions.erase(found);
            }
        }
    }

    void acquire(const fs::path& storage, const UserRecord& challenge)
    {
        ensure(!key_, "connection is already authenticated");
        SessionKey key{fs::canonical(storage), challenge.name};
        std::lock_guard<std::mutex> lock(gStorageMutex);
        auto users = loadUsers(storage);
        ensure(std::any_of(users.begin(), users.end(),
                           [&](const auto& user)
                           {
                               return user.name == challenge.name &&
                                      user.salt == challenge.salt &&
                                      user.verifier == challenge.verifier;
                           }),
               "account changed during login; retry");
        auto entry = activeSessions.try_emplace(key, 0).first;
        key_ = std::move(key);
        ++entry->second;
    }

    void deleteAccount(const fs::path& storage, std::atomic_bool* cancel)
    {
        std::lock_guard<std::mutex> lock(gStorageMutex);
        ensure(key_ && activeSessions.at(*key_) == 1,
               "账户正在使用，请稍后重试");
        auto users = loadUsers(storage);
        auto user = std::find_if(users.begin(), users.end(), [&](const auto& u)
                                 { return u.name == key_->second; });
        ensure(user != users.end(), "account no longer exists");
        const auto directory = userDirectory(storage, user->name);
        // is_empty throws on unreadable storage; never mistake it for empty.
        ensure(!fs::exists(directory) || fs::is_empty(directory),
               "账户有云端备份，不能注销");
        checkCancelled(cancel);
        users.erase(user);
        saveUsers(storage, users);
        // Empty storage directories are harmless; do not delete any files.
    }

private:
    std::optional<SessionKey> key_;
};

void handleUpload(int fd, uint32_t requestId, const Frame& start,
                  const fs::path& storage, const std::string& username,
                  std::atomic_bool* cancel)
{
    Reader reader(start.payload);
    std::string displayName = reader.string();
    uint64_t declaredSize = reader.u64();
    auto digestBytes = reader.bytes(32);
    reader.end();
    ensure(displayName.size() <= 256, "backup display name is too long");
    std::array<uint8_t, 32> declaredDigest{};
    std::copy(digestBytes.begin(), digestBytes.end(), declaredDigest.begin());
    fs::path dir = userDirectory(storage, username);
    fs::create_directories(dir);
    ensure(chmod(dir.c_str(), 0700) == 0,
           "cannot secure user storage directory");
    std::string id = randomId();
    while (fs::exists(dir / (id + ".bak")))
    {
        id = randomId();
    }
    backup::detail::TempFile staging(dir);
    const fs::path& temp = staging.path();
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    ensure(out.good(), "cannot create upload staging file");
    chmod(temp.c_str(), 0600);
    std::vector<uint8_t> ready;
    putString(ready, id);
    sendFrame(fd, MessageType::UploadReady, requestId, ready, cancel);
    uint64_t received = 0;
    while (true)
    {
        auto frame = receiveFrame(fd, cancel);
        ensure(frame.has_value(), "client disconnected during upload");
        ensure(frame->requestId == requestId,
               "upload request identifier changed");
        if (frame->type == MessageType::UploadEnd)
        {
            ensure(frame->payload.empty(), "upload end has trailing data");
            break;
        }
        ensure(frame->type == MessageType::UploadChunk,
               "unexpected message during upload");
        ensure(frame->payload.size() <= declaredSize - received,
               "upload exceeds declared size");
        if (!frame->payload.empty())
        {
            out.write(reinterpret_cast<const char*>(frame->payload.data()),
                      static_cast<std::streamsize>(frame->payload.size()));
        }
        ensure(out.good(), "cannot store upload chunk");
        received += frame->payload.size();
    }
    out.close();
    ensure(out.good(), "cannot close uploaded archive");
    ensure(received == declaredSize,
           "uploaded size does not match declaration");
    ensure(sha256File(temp.string(), 0, UINT64_MAX, cancel) == declaredDigest,
           "uploaded SHA-256 does not match declaration");
    fs::path final = dir / (id + ".bak");
    checkCancelled(cancel);
    std::error_code ec;
    commitStoredFile(temp, final);
    RemoteBackupEntry entry;
    entry.id = id;
    entry.name = displayName.empty() ? id : displayName;
    entry.timestamp = static_cast<uint64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    entry.size = declaredSize;
    entry.digest = hexDigest(declaredDigest);
    try
    {
        writeMeta(dir / (id + ".meta"), entry);
    }
    catch (...)
    {
        // Metadata may have been published before its directory fsync failed.
        // Remove it first; if removal fails, retain the matching archive.
        fs::remove(dir / (id + ".meta"), ec);
        if (!ec)
        {
            fs::remove(final, ec);
        }
        throw;
    }
    std::vector<uint8_t> result;
    putString(result, id);
    sendFrame(fd, MessageType::UploadResult, requestId, result, cancel);
}

void handleDownload(int fd, uint32_t requestId, const Frame& request,
                    const fs::path& storage, const std::string& username,
                    std::atomic_bool* cancel)
{
    Reader reader(request.payload);
    std::string id = reader.string();
    reader.end();
    ensure(validBackupId(id), "invalid backup identifier");
    fs::path dir = userDirectory(storage, username);
    fs::path archive = dir / (id + ".bak");
    fs::path meta = dir / (id + ".meta");
    ensure(fs::exists(archive) && fs::exists(meta),
           "backup was not found for this user");
    auto entry = readMeta(meta, id);
    auto digest = sha256File(archive.string(), 0, UINT64_MAX, cancel);
    ensure(hexDigest(digest) == entry.digest,
           "stored backup checksum mismatch");
    std::vector<uint8_t> start;
    putU64(start, entry.size);
    start.insert(start.end(), digest.begin(), digest.end());
    sendFrame(fd, MessageType::DownloadStart, requestId, start, cancel);
    std::ifstream in(archive, std::ios::binary);
    ensure(in.good(), "cannot open stored backup");
    std::vector<uint8_t> chunk(kChunkSize);
    while (in)
    {
        in.read(reinterpret_cast<char*>(chunk.data()),
                static_cast<std::streamsize>(chunk.size()));
        size_t got = static_cast<size_t>(in.gcount());
        if (got)
        {
            sendFrame(
                fd, MessageType::DownloadChunk, requestId,
                {chunk.begin(), chunk.begin() + static_cast<ptrdiff_t>(got)},
                cancel);
        }
    }
    ensure(in.eof(), "cannot read stored backup");
    sendFrame(fd, MessageType::DownloadEnd, requestId, {}, cancel);
}

} // namespace

void handleSession(int fd, const ServerConfig& config, std::atomic_bool* cancel)
{
    Socket socket(fd);
    setTimeouts(fd, config.timeoutSeconds);
    std::string authenticatedUser;
    AuthenticatedSession session;
    std::optional<UserRecord> pendingUser;
    std::vector<uint8_t> pendingNonce;
    uint32_t currentRequest = 0;
    uint32_t challengeRequest = 0;
    try
    {
        while (true)
        {
            auto maybe = receiveFrame(fd, cancel);
            if (!maybe)
            {
                return;
            }
            Frame frame = std::move(*maybe);
            currentRequest = frame.requestId;
            if (frame.type == MessageType::RegisterRequest)
            {
                Reader r(frame.payload);
                std::string name = r.string();
                auto salt = r.bytes(16);
                auto verifier = r.bytes(32);
                r.end();
                ensure(validUsername(name), "invalid username");
                std::lock_guard<std::mutex> lock(gStorageMutex);
                auto users = loadUsers(config.storagePath);
                ensure(std::none_of(users.begin(), users.end(),
                                    [&](const auto& u)
                                    { return u.name == name; }),
                       "username already exists");
                UserRecord u;
                u.name = name;
                std::copy(salt.begin(), salt.end(), u.salt.begin());
                std::copy(verifier.begin(), verifier.end(), u.verifier.begin());
                users.push_back(u);
                saveUsers(config.storagePath, users);
                std::vector<uint8_t> ok{0};
                sendFrame(fd, MessageType::RegisterResponse, frame.requestId,
                          ok, cancel);
                continue;
            }
            if (frame.type == MessageType::LoginStart)
            {
                Reader r(frame.payload);
                std::string name = r.string();
                r.end();
                ensure(validUsername(name), "invalid username");
                pendingUser = findUser(config.storagePath, name);
                ensure(pendingUser.has_value(), "unknown username");
                pendingNonce = randomBytes(16);
                challengeRequest = frame.requestId;
                std::vector<uint8_t> challenge(pendingUser->salt.begin(),
                                               pendingUser->salt.end());
                challenge.insert(challenge.end(), pendingNonce.begin(),
                                 pendingNonce.end());
                sendFrame(fd, MessageType::LoginChallenge, frame.requestId,
                          challenge, cancel);
                continue;
            }
            if (frame.type == MessageType::LoginProof)
            {
                ensure(pendingUser.has_value() &&
                           frame.requestId == challengeRequest,
                       "login proof has no matching challenge");
                ensure(frame.payload.size() == 32, "invalid login proof size");
                auto expected = proofFor(pendingUser->verifier, pendingNonce);
                ensure(std::equal(expected.begin(), expected.end(),
                                  frame.payload.begin()),
                       "invalid username or password");
                authenticatedUser = pendingUser->name;
                session.acquire(config.storagePath, *pendingUser);
                pendingUser.reset();
                pendingNonce.clear();
                sendFrame(fd, MessageType::LoginResponse, frame.requestId, {0},
                          cancel);
                continue;
            }
            ensure(!authenticatedUser.empty(), "authentication is required");
            if (frame.type == MessageType::UploadStart)
            {
                handleUpload(fd, frame.requestId, frame, config.storagePath,
                             authenticatedUser, cancel);
            }
            else if (frame.type == MessageType::ListRequest)
            {
                Reader r(frame.payload);
                r.end();
                auto entries =
                    listEntries(config.storagePath, authenticatedUser);
                ensure(entries.size() <= 4096,
                       "backup list exceeds protocol limit");
                std::vector<uint8_t> payload;
                putU32(payload, static_cast<uint32_t>(entries.size()));
                for (const auto& e : entries)
                {
                    putString(payload, e.id);
                    putString(payload, e.name);
                    putU64(payload, e.timestamp);
                    putU64(payload, e.size);
                    putString(payload, e.digest);
                    ensure(payload.size() <= kMaxPayload,
                           "backup list exceeds frame size limit");
                }
                sendFrame(fd, MessageType::ListResponse, frame.requestId,
                          payload, cancel);
            }
            else if (frame.type == MessageType::DownloadRequest)
            {
                handleDownload(fd, frame.requestId, frame, config.storagePath,
                               authenticatedUser, cancel);
            }
            else if (frame.type == MessageType::DeleteAccountRequest)
            {
                Reader reader(frame.payload);
                reader.end();
                session.deleteAccount(config.storagePath, cancel);
                sendFrame(fd, MessageType::DeleteAccountResponse,
                          frame.requestId, {0}, cancel);
                return;
            }
            else
            {
                throw NetError("unsupported network command");
            }
        }
    }
    catch (const std::exception& e)
    {
        if (cancel && cancel->load())
        {
            return;
        }
        try
        {
            sendError(fd, currentRequest, e.what());
        }
        catch (...)
        {
        }
    }
}

} // namespace backup::network::detail
