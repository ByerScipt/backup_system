#include "backup/network.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace backup::network {
namespace {

constexpr uint16_t kProtocolVersion = 1;
constexpr uint32_t kMaxPayload = 1024 * 1024;
constexpr size_t kChunkSize = 512 * 1024;
constexpr std::array<char,4> kMagic{{'N','B','K','P'}};

enum class MessageType : uint16_t {
    RegisterRequest = 1, RegisterResponse = 2,
    LoginStart = 3, LoginChallenge = 4, LoginProof = 5, LoginResponse = 6,
    UploadStart = 10, UploadReady = 11, UploadChunk = 12, UploadEnd = 13, UploadResult = 14,
    ListRequest = 20, ListResponse = 21,
    DownloadRequest = 30, DownloadStart = 31, DownloadChunk = 32, DownloadEnd = 33,
    Error = 255
};

struct NetError : std::runtime_error { using std::runtime_error::runtime_error; };

void ensure(bool condition, const std::string& message) {
    if (!condition) throw NetError(message);
}

void checkCancelled(std::atomic_bool* cancel) {
    ensure(cancel == nullptr || !cancel->load(), "network operation cancelled");
}

void reportProgress(const ProgressCallback& progress, const std::string& stage,
                    uint64_t completed, uint64_t total) {
    if (progress) progress({stage,completed,total,{}});
}

void commitFileNoReplace(const fs::path& temporary, const fs::path& output) {
#if defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD,
                  output.c_str(), RENAME_NOREPLACE) == 0) return;
    if (errno != ENOSYS && errno != EINVAL)
        throw NetError("cannot commit output without replacement: " + std::string(std::strerror(errno)));
#endif
    ensure(::link(temporary.c_str(),output.c_str())==0,
           "cannot commit output without replacement: "+std::string(std::strerror(errno)));
    static_cast<void>(::unlink(temporary.c_str()));
}

class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() { close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) { close(); fd_ = other.fd_; other.fd_ = -1; }
        return *this;
    }
    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    int release() { int value = fd_; fd_ = -1; return value; }
    void close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
private:
    int fd_ = -1;
};

void sendAll(int fd, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        ensure(n > 0, "network send failed");
        sent += static_cast<size_t>(n);
    }
}

bool receiveAll(int fd, uint8_t* data, size_t size, bool allowCleanEof = false) {
    size_t received = 0;
    while (received < size) {
        ssize_t n = ::recv(fd, data + received, size - received, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0 && received == 0 && allowCleanEof) return false;
        ensure(n > 0, "network connection closed unexpectedly");
        received += static_cast<size_t>(n);
    }
    return true;
}

void putU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8)); out.push_back(static_cast<uint8_t>(value));
}
void putU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
void putU64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
void putString(std::vector<uint8_t>& out, const std::string& value) {
    ensure(value.size() <= 65535, "network string is too long");
    putU16(out, static_cast<uint16_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& data) : data_(data) {}
    uint8_t u8() { need(1); return data_[offset_++]; }
    uint16_t u16() { need(2); uint16_t v=(data_[offset_]<<8)|data_[offset_+1]; offset_+=2; return v; }
    uint32_t u32() { need(4); uint32_t v=0; for(int i=0;i<4;++i)v=(v<<8)|data_[offset_++]; return v; }
    uint64_t u64() { need(8); uint64_t v=0; for(int i=0;i<8;++i)v=(v<<8)|data_[offset_++]; return v; }
    std::string string() {
        uint16_t n=u16(); need(n); std::string v(reinterpret_cast<const char*>(data_.data()+offset_),n);
        offset_+=n; ensure(v.find('\0')==std::string::npos,"network string contains NUL"); return v;
    }
    std::vector<uint8_t> bytes(size_t n) { need(n); auto a=data_.begin()+static_cast<ptrdiff_t>(offset_); offset_+=n; return {a,a+static_cast<ptrdiff_t>(n)}; }
    void end() const { ensure(offset_==data_.size(),"network message has trailing data"); }
private:
    const std::vector<uint8_t>& data_; size_t offset_=0;
    void need(size_t n) const { ensure(n<=data_.size()-std::min(offset_,data_.size()),"truncated network message"); }
};

struct Frame {
    MessageType type = MessageType::Error;
    uint32_t requestId = 0;
    std::vector<uint8_t> payload;
};

void sendFrame(int fd, MessageType type, uint32_t requestId, const std::vector<uint8_t>& payload = {}) {
    ensure(payload.size() <= kMaxPayload, "network frame exceeds size limit");
    std::vector<uint8_t> header;
    header.insert(header.end(), kMagic.begin(), kMagic.end());
    putU16(header, kProtocolVersion); putU16(header, static_cast<uint16_t>(type));
    putU32(header, requestId); putU32(header, static_cast<uint32_t>(payload.size()));
    sendAll(fd, header.data(), header.size());
    if (!payload.empty()) sendAll(fd, payload.data(), payload.size());
}

std::optional<Frame> receiveFrame(int fd) {
    std::array<uint8_t,16> header{};
    if (!receiveAll(fd, header.data(), header.size(), true)) return std::nullopt;
    ensure(std::equal(kMagic.begin(), kMagic.end(), header.begin()), "invalid network frame magic");
    std::vector<uint8_t> hv(header.begin()+4,header.end()); Reader reader(hv);
    ensure(reader.u16()==kProtocolVersion,"network protocol version mismatch");
    Frame frame; frame.type=static_cast<MessageType>(reader.u16()); frame.requestId=reader.u32();
    uint32_t size=reader.u32(); reader.end(); ensure(size<=kMaxPayload,"network payload exceeds size limit");
    frame.payload.resize(size); if(size)receiveAll(fd,frame.payload.data(),size);
    return frame;
}

void sendError(int fd, uint32_t requestId, const std::string& message) {
    std::vector<uint8_t> payload; putString(payload,message); sendFrame(fd,MessageType::Error,requestId,payload);
}

std::string errorFromFrame(const Frame& frame) {
    Reader r(frame.payload); std::string value=r.string(); r.end(); return value;
}

Socket connectTo(const std::string& host, uint16_t port) {
    addrinfo hints{}; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    addrinfo* result=nullptr; std::string service=std::to_string(port);
    ensure(getaddrinfo(host.c_str(),service.c_str(),&hints,&result)==0,"cannot resolve server address");
    Socket socket;
    for(addrinfo* p=result;p;p=p->ai_next){
        int fd=::socket(p->ai_family,p->ai_socktype,p->ai_protocol); if(fd<0)continue;
        if(::connect(fd,p->ai_addr,p->ai_addrlen)==0){socket=Socket(fd);break;} ::close(fd);
    }
    freeaddrinfo(result); ensure(socket.valid(),"cannot connect to backup server"); return socket;
}

std::vector<uint8_t> randomBytes(size_t count) {
    std::random_device random; std::vector<uint8_t> out(count); for(auto& b:out)b=static_cast<uint8_t>(random()); return out;
}

std::array<uint8_t,32> verifierFor(const std::string& password, const std::vector<uint8_t>& salt) {
    std::vector<uint8_t> material(password.begin(),password.end()); material.insert(material.end(),salt.begin(),salt.end());
    auto digest=sha256(material);
    for(int i=0;i<4096;++i){std::vector<uint8_t> next(digest.begin(),digest.end());next.insert(next.end(),salt.begin(),salt.end());digest=sha256(next);} return digest;
}

std::array<uint8_t,32> proofFor(const std::array<uint8_t,32>& verifier, const std::vector<uint8_t>& nonce) {
    std::vector<uint8_t> data(verifier.begin(),verifier.end()); data.insert(data.end(),nonce.begin(),nonce.end()); return sha256(data);
}

bool validUsername(const std::string& value) {
    if(value.empty()||value.size()>64)return false;
    return std::all_of(value.begin(),value.end(),[](unsigned char c){return std::isalnum(c)||c=='_'||c=='-'||c=='.';});
}

bool validBackupId(const std::string& value) {
    return value.size()==32&&std::all_of(value.begin(),value.end(),[](unsigned char c){return std::isxdigit(c);});
}

std::string randomId() {
    auto bytes=randomBytes(16); std::ostringstream out; out<<std::hex;
    for(uint8_t b:bytes){out.width(2);out.fill('0');out<<static_cast<unsigned>(b);} return out.str();
}

struct UserRecord { std::string name; std::array<uint8_t,16> salt{}; std::array<uint8_t,32> verifier{}; };
std::mutex gStorageMutex;

std::vector<UserRecord> loadUsers(const fs::path& storage) {
    std::vector<UserRecord> users; fs::path path=storage/"users.db"; std::ifstream in(path,std::ios::binary); if(!in)return users;
    std::array<char,4> magic{}; in.read(magic.data(),4); ensure(in.good()&&magic==std::array<char,4>{{'U','S','R','1'}},"invalid user database");
    uint8_t b[4]{}; in.read(reinterpret_cast<char*>(b),4); ensure(in.good(),"truncated user database");
    uint32_t count=static_cast<uint32_t>(b[0])|(static_cast<uint32_t>(b[1])<<8)|(static_cast<uint32_t>(b[2])<<16)|(static_cast<uint32_t>(b[3])<<24);
    ensure(count<=100000,"user database count exceeds limit");
    for(uint32_t i=0;i<count;++i){
        uint8_t nbuf[2]{};in.read(reinterpret_cast<char*>(nbuf),2);ensure(in.good(),"truncated user database");uint16_t n=nbuf[0]|(nbuf[1]<<8);
        UserRecord u;u.name.resize(n);in.read(u.name.data(),n);in.read(reinterpret_cast<char*>(u.salt.data()),u.salt.size());in.read(reinterpret_cast<char*>(u.verifier.data()),u.verifier.size());ensure(in.good()&&validUsername(u.name),"invalid user database record");users.push_back(u);
    } return users;
}

void saveUsers(const fs::path& storage, const std::vector<UserRecord>& users) {
    fs::create_directories(storage); fs::path temp=storage/(".users-"+randomId()+".tmp"); std::ofstream out(temp,std::ios::binary|std::ios::trunc);ensure(out.good(),"cannot write user database");
    ensure(chmod(temp.c_str(),0600)==0,"cannot secure user database permissions");
    out.write("USR1",4);uint32_t count=static_cast<uint32_t>(users.size());uint8_t cb[4]{static_cast<uint8_t>(count),static_cast<uint8_t>(count>>8),static_cast<uint8_t>(count>>16),static_cast<uint8_t>(count>>24)};out.write(reinterpret_cast<char*>(cb),4);
    for(const auto& u:users){uint16_t n=static_cast<uint16_t>(u.name.size());uint8_t nb[2]{static_cast<uint8_t>(n),static_cast<uint8_t>(n>>8)};out.write(reinterpret_cast<char*>(nb),2);out.write(u.name.data(),n);out.write(reinterpret_cast<const char*>(u.salt.data()),u.salt.size());out.write(reinterpret_cast<const char*>(u.verifier.data()),u.verifier.size());}
    out.close();ensure(out.good(),"cannot finalize user database");std::error_code ec;fs::rename(temp,storage/"users.db",ec);if(ec)fs::remove(temp);ensure(!ec,"cannot commit user database");
}

fs::path userDirectory(const fs::path& storage, const std::string& username) {
    return storage/"users"/hexDigest(sha256(username));
}

void writeMeta(const fs::path& path, const RemoteBackupEntry& entry) {
    fs::path temp=path.string()+".tmp-"+randomId();std::ofstream out(temp,std::ios::binary|std::ios::trunc);ensure(out.good(),"cannot write backup metadata");
    ensure(chmod(temp.c_str(),0600)==0,"cannot secure backup metadata permissions");
    out.write("MET1",4);uint16_t n=static_cast<uint16_t>(entry.name.size());uint8_t nb[2]{static_cast<uint8_t>(n),static_cast<uint8_t>(n>>8)};out.write(reinterpret_cast<char*>(nb),2);out.write(entry.name.data(),n);
    auto little64=[&](uint64_t v){uint8_t b[8];for(int i=0;i<8;++i)b[i]=static_cast<uint8_t>(v>>(i*8));out.write(reinterpret_cast<char*>(b),8);};little64(entry.timestamp);little64(entry.size);out.write(entry.digest.data(),static_cast<std::streamsize>(entry.digest.size()));out.close();ensure(out.good(),"cannot finalize backup metadata");
    std::error_code ec;fs::rename(temp,path,ec);if(ec)fs::remove(temp);ensure(!ec,"cannot commit backup metadata");
}

RemoteBackupEntry readMeta(const fs::path& path, const std::string& id) {
    std::ifstream in(path,std::ios::binary);ensure(in.good(),"cannot read backup metadata");std::array<char,4> magic{};in.read(magic.data(),4);ensure(in.good()&&magic==std::array<char,4>{{'M','E','T','1'}},"invalid backup metadata");
    uint8_t nb[2]{};in.read(reinterpret_cast<char*>(nb),2);uint16_t n=nb[0]|(nb[1]<<8);ensure(n<=4096,"backup name is too long");RemoteBackupEntry e;e.id=id;e.name.resize(n);in.read(e.name.data(),n);
    auto little64=[&](){uint8_t b[8]{};in.read(reinterpret_cast<char*>(b),8);uint64_t v=0;for(int i=0;i<8;++i)v|=static_cast<uint64_t>(b[i])<<(i*8);return v;};e.timestamp=little64();e.size=little64();e.digest.resize(64);in.read(e.digest.data(),64);ensure(in.good(),"truncated backup metadata");return e;
}

std::vector<RemoteBackupEntry> listEntries(const fs::path& storage, const std::string& username) {
    std::vector<RemoteBackupEntry> entries;fs::path dir=userDirectory(storage,username);std::error_code ec;if(!fs::exists(dir,ec))return entries;
    for(const auto& item:fs::directory_iterator(dir,ec)){ensure(!ec,"cannot enumerate user backups");std::string name=item.path().filename().string();if(name.size()==37&&name.substr(32)==".meta"){std::string id=name.substr(0,32);if(validBackupId(id))entries.push_back(readMeta(item.path(),id));}}
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.timestamp>b.timestamp;});return entries;
}

std::optional<UserRecord> findUser(const fs::path& storage, const std::string& name) {
    std::lock_guard<std::mutex> lock(gStorageMutex);for(const auto& user:loadUsers(storage))if(user.name==name)return user;return std::nullopt;
}

void setTimeouts(int fd, uint32_t seconds) {
    timeval timeout{static_cast<time_t>(seconds),0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
}

bool authenticateClient(int fd, uint32_t& requestId, const std::string& username,
                        const std::string& password, std::string& error) {
    try {
        std::vector<uint8_t> start;putString(start,username);sendFrame(fd,MessageType::LoginStart,++requestId,start);
        auto challenge=receiveFrame(fd);ensure(challenge.has_value(),"server closed during login");if(challenge->type==MessageType::Error)throw NetError(errorFromFrame(*challenge));ensure(challenge->type==MessageType::LoginChallenge&&challenge->requestId==requestId,"unexpected login challenge");
        Reader reader(challenge->payload);auto salt=reader.bytes(16);auto nonce=reader.bytes(16);reader.end();auto verifier=verifierFor(password,salt);auto proof=proofFor(verifier,nonce);std::vector<uint8_t> payload(proof.begin(),proof.end());sendFrame(fd,MessageType::LoginProof,requestId,payload);
        auto response=receiveFrame(fd);ensure(response.has_value(),"server closed during login");if(response->type==MessageType::Error)throw NetError(errorFromFrame(*response));ensure(response->type==MessageType::LoginResponse&&response->requestId==requestId,"unexpected login response");Reader status(response->payload);ensure(status.u8()==0,"login rejected");status.end();return true;
    } catch(const std::exception& e){error=e.what();return false;}
}

void handleUpload(int fd, uint32_t requestId, const Frame& start, const fs::path& storage,
                  const std::string& username) {
    Reader reader(start.payload);std::string displayName=reader.string();uint64_t declaredSize=reader.u64();auto digestBytes=reader.bytes(32);reader.end();ensure(displayName.size()<=256,"backup display name is too long");
    std::array<uint8_t,32> declaredDigest{};std::copy(digestBytes.begin(),digestBytes.end(),declaredDigest.begin());
    fs::path dir=userDirectory(storage,username);fs::create_directories(dir);ensure(chmod(dir.c_str(),0700)==0,"cannot secure user storage directory");std::string id=randomId();while(fs::exists(dir/(id+".bak")))id=randomId();fs::path temp=dir/(".upload-"+id+".tmp");
    struct Cleanup{fs::path path;bool keep=false;~Cleanup(){if(!keep){std::error_code ec;fs::remove(path,ec);}}} cleanup{temp};
    std::ofstream out(temp,std::ios::binary|std::ios::trunc);ensure(out.good(),"cannot create upload staging file");chmod(temp.c_str(),0600);std::vector<uint8_t> ready;putString(ready,id);sendFrame(fd,MessageType::UploadReady,requestId,ready);
    uint64_t received=0;
    while(true){auto frame=receiveFrame(fd);ensure(frame.has_value(),"client disconnected during upload");ensure(frame->requestId==requestId,"upload request identifier changed");if(frame->type==MessageType::UploadEnd)break;ensure(frame->type==MessageType::UploadChunk,"unexpected message during upload");ensure(frame->payload.size()<=declaredSize-received,"upload exceeds declared size");if(!frame->payload.empty())out.write(reinterpret_cast<const char*>(frame->payload.data()),static_cast<std::streamsize>(frame->payload.size()));ensure(out.good(),"cannot store upload chunk");received+=frame->payload.size();}
    out.close();ensure(received==declaredSize,"uploaded size does not match declaration");ensure(sha256File(temp.string())==declaredDigest,"uploaded SHA-256 does not match declaration");fs::path final=dir/(id+".bak");std::error_code ec;fs::rename(temp,final,ec);ensure(!ec,"cannot commit uploaded archive");cleanup.keep=true;
    RemoteBackupEntry entry;entry.id=id;entry.name=displayName.empty()?id:displayName;entry.timestamp=static_cast<uint64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));entry.size=declaredSize;entry.digest=hexDigest(declaredDigest);
    try { writeMeta(dir/(id+".meta"),entry); }
    catch (...) { fs::remove(final,ec); throw; }
    std::vector<uint8_t> result;putString(result,id);sendFrame(fd,MessageType::UploadResult,requestId,result);
}

void handleDownload(int fd, uint32_t requestId, const Frame& request, const fs::path& storage,
                    const std::string& username) {
    Reader reader(request.payload);std::string id=reader.string();reader.end();ensure(validBackupId(id),"invalid backup identifier");fs::path dir=userDirectory(storage,username);fs::path archive=dir/(id+".bak");fs::path meta=dir/(id+".meta");ensure(fs::exists(archive)&&fs::exists(meta),"backup was not found for this user");auto entry=readMeta(meta,id);auto digest=sha256File(archive.string());ensure(hexDigest(digest)==entry.digest,"stored backup checksum mismatch");
    std::vector<uint8_t> start;putU64(start,entry.size);start.insert(start.end(),digest.begin(),digest.end());sendFrame(fd,MessageType::DownloadStart,requestId,start);std::ifstream in(archive,std::ios::binary);ensure(in.good(),"cannot open stored backup");std::vector<uint8_t> chunk(kChunkSize);while(in){in.read(reinterpret_cast<char*>(chunk.data()),static_cast<std::streamsize>(chunk.size()));size_t got=static_cast<size_t>(in.gcount());if(got)sendFrame(fd,MessageType::DownloadChunk,requestId,{chunk.begin(),chunk.begin()+static_cast<ptrdiff_t>(got)});}sendFrame(fd,MessageType::DownloadEnd,requestId);
}

void handleSession(int fd, ServerConfig config) {
    Socket socket(fd);setTimeouts(fd,config.timeoutSeconds);std::string authenticatedUser;std::optional<UserRecord> pendingUser;std::vector<uint8_t> pendingNonce;uint32_t pendingRequest=0;
    try {
        while(true){auto maybe=receiveFrame(fd);if(!maybe)return;Frame frame=std::move(*maybe);pendingRequest=frame.requestId;
            if(frame.type==MessageType::RegisterRequest){Reader r(frame.payload);std::string name=r.string();auto salt=r.bytes(16);auto verifier=r.bytes(32);r.end();ensure(validUsername(name),"invalid username");std::lock_guard<std::mutex> lock(gStorageMutex);auto users=loadUsers(config.storagePath);ensure(std::none_of(users.begin(),users.end(),[&](const auto& u){return u.name==name;}),"username already exists");UserRecord u;u.name=name;std::copy(salt.begin(),salt.end(),u.salt.begin());std::copy(verifier.begin(),verifier.end(),u.verifier.begin());users.push_back(u);saveUsers(config.storagePath,users);std::vector<uint8_t> ok{0};sendFrame(fd,MessageType::RegisterResponse,frame.requestId,ok);continue;}
            if(frame.type==MessageType::LoginStart){Reader r(frame.payload);std::string name=r.string();r.end();ensure(validUsername(name),"invalid username");pendingUser=findUser(config.storagePath,name);ensure(pendingUser.has_value(),"unknown username");pendingNonce=randomBytes(16);pendingRequest=frame.requestId;std::vector<uint8_t> challenge(pendingUser->salt.begin(),pendingUser->salt.end());challenge.insert(challenge.end(),pendingNonce.begin(),pendingNonce.end());sendFrame(fd,MessageType::LoginChallenge,frame.requestId,challenge);continue;}
            if(frame.type==MessageType::LoginProof){ensure(pendingUser.has_value()&&frame.requestId==pendingRequest,"login proof has no matching challenge");ensure(frame.payload.size()==32,"invalid login proof size");auto expected=proofFor(pendingUser->verifier,pendingNonce);ensure(std::equal(expected.begin(),expected.end(),frame.payload.begin()),"invalid username or password");authenticatedUser=pendingUser->name;pendingUser.reset();pendingNonce.clear();sendFrame(fd,MessageType::LoginResponse,frame.requestId,{0});continue;}
            ensure(!authenticatedUser.empty(),"authentication is required");
            if(frame.type==MessageType::UploadStart)handleUpload(fd,frame.requestId,frame,config.storagePath,authenticatedUser);
            else if(frame.type==MessageType::ListRequest){Reader r(frame.payload);r.end();auto entries=listEntries(config.storagePath,authenticatedUser);ensure(entries.size()<=4096,"backup list exceeds protocol limit");std::vector<uint8_t> payload;putU32(payload,static_cast<uint32_t>(entries.size()));for(const auto& e:entries){putString(payload,e.id);putString(payload,e.name);putU64(payload,e.timestamp);putU64(payload,e.size);putString(payload,e.digest);ensure(payload.size()<=kMaxPayload,"backup list exceeds frame size limit");}sendFrame(fd,MessageType::ListResponse,frame.requestId,payload);}
            else if(frame.type==MessageType::DownloadRequest)handleDownload(fd,frame.requestId,frame,config.storagePath,authenticatedUser);
            else throw NetError("unsupported network command");
        }
    } catch(const std::exception& e){try{sendError(fd,pendingRequest,e.what());}catch(...){}}
}

} // namespace

ServerConfig ServerConfig::load(const std::string& path) {
    ServerConfig config;std::ifstream in(path);if(!in)throw std::runtime_error("cannot open server config: "+path);std::string line;
    auto trim=[](std::string value){auto first=value.find_first_not_of(" \t\r\n");auto last=value.find_last_not_of(" \t\r\n");return first==std::string::npos?std::string():value.substr(first,last-first+1);};
    while(std::getline(in,line)){line=trim(line);if(line.empty()||line[0]=='#'||line[0]==';'||line[0]=='[')continue;auto equals=line.find('=');if(equals==std::string::npos)continue;std::string key=trim(line.substr(0,equals)),value=trim(line.substr(equals+1));if(key=="port")config.port=static_cast<uint16_t>(std::stoul(value));else if(key=="storage_path")config.storagePath=value;else if(key=="max_connections")config.maxConnections=static_cast<uint32_t>(std::stoul(value));else if(key=="timeout_seconds")config.timeoutSeconds=static_cast<uint32_t>(std::stoul(value));}
    if(config.port==0||config.maxConnections==0||config.maxConnections>1024||config.timeoutSeconds==0)
        throw std::runtime_error("invalid server configuration");
    return config;
}

BackupServer::BackupServer(ServerConfig config) : config_(std::move(config)) {}

bool BackupServer::run() {
    struct Session {
        std::thread worker;
        std::shared_ptr<std::atomic_bool> finished;
    };
    std::vector<Session> sessions;
    auto reap = [&](bool all) {
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (all || it->finished->load()) {
                if (it->worker.joinable()) it->worker.join();
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
    };
    bool success = false;
    try {
        fs::create_directories(config_.storagePath);
        int fd=::socket(AF_INET,SOCK_STREAM,0);ensure(fd>=0,"cannot create listening socket");
        listenFd_.store(fd);int yes=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=INADDR_ANY;
        address.sin_port=htons(config_.port);
        ensure(::bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,
               "cannot bind backup server port");
        ensure(::listen(fd,static_cast<int>(config_.maxConnections))==0,
               "cannot listen on backup server port");
        running_=true;
        std::cout<<"backup-server listening on 0.0.0.0:"<<config_.port<<"\n";
        while(running_){
            int client=::accept(fd,nullptr,nullptr);
            if(client<0){if(errno==EINTR)continue;if(!running_)break;continue;}
            reap(false);
            if(sessions.size()>=config_.maxConnections){
                try{sendError(client,0,"server connection limit reached");}catch(...){}
                ::close(client);continue;
            }
            auto finished=std::make_shared<std::atomic_bool>(false);
            sessions.push_back({
                std::thread([client,config=config_,finished](){
                    handleSession(client,config);finished->store(true);
                }),
                std::move(finished)
            });
        }
        success = true;
    } catch(const std::exception& e){
        std::cerr<<"Server error: "<<e.what()<<"\n";
    }
    running_=false;
    int listening = listenFd_.exchange(-1);
    if(listening>=0) ::close(listening);
    reap(true);
    return success;
}

void BackupServer::stop() {
    running_=false;
    int listening=listenFd_.exchange(-1);
    if(listening>=0){::shutdown(listening,SHUT_RDWR);::close(listening);}
}

BackupClient::BackupClient(std::string host,uint16_t port,std::string username,std::string password)
    :host_(std::move(host)),port_(port),username_(std::move(username)),password_(std::move(password)){}

bool BackupClient::registerUser(std::string& error) {
    try{ensure(validUsername(username_),"invalid username");ensure(!password_.empty(),"account password is required");Socket socket=connectTo(host_,port_);auto salt=randomBytes(16);auto verifier=verifierFor(password_,salt);std::vector<uint8_t> payload;putString(payload,username_);payload.insert(payload.end(),salt.begin(),salt.end());payload.insert(payload.end(),verifier.begin(),verifier.end());sendFrame(socket.get(),MessageType::RegisterRequest,1,payload);auto response=receiveFrame(socket.get());ensure(response.has_value(),"server closed during registration");if(response->type==MessageType::Error)throw NetError(errorFromFrame(*response));ensure(response->type==MessageType::RegisterResponse,"unexpected registration response");Reader r(response->payload);ensure(r.u8()==0,"registration rejected");r.end();return true;}catch(const std::exception& e){error=e.what();return false;}
}

bool BackupClient::upload(const std::string& archivePath,const std::string& displayName,
                          std::string& backupId,std::string& error,
                          ProgressCallback progress,std::atomic_bool* cancel) {
    try{checkCancelled(cancel);Socket socket=connectTo(host_,port_);uint32_t request=0;ensure(authenticateClient(socket.get(),request,username_,password_,error),error);checkCancelled(cancel);uint64_t size=static_cast<uint64_t>(fs::file_size(archivePath));auto digest=sha256File(archivePath);checkCancelled(cancel);std::vector<uint8_t> start;putString(start,displayName);putU64(start,size);start.insert(start.end(),digest.begin(),digest.end());sendFrame(socket.get(),MessageType::UploadStart,++request,start);auto ready=receiveFrame(socket.get());ensure(ready.has_value(),"server closed before upload");if(ready->type==MessageType::Error)throw NetError(errorFromFrame(*ready));ensure(ready->type==MessageType::UploadReady&&ready->requestId==request,"unexpected upload response");Reader rr(ready->payload);backupId=rr.string();rr.end();std::ifstream in(archivePath,std::ios::binary);ensure(in.good(),"cannot open archive for upload");std::vector<uint8_t> chunk(kChunkSize);uint64_t sent=0;while(in){checkCancelled(cancel);in.read(reinterpret_cast<char*>(chunk.data()),static_cast<std::streamsize>(chunk.size()));size_t got=static_cast<size_t>(in.gcount());if(got){sendFrame(socket.get(),MessageType::UploadChunk,request,{chunk.begin(),chunk.begin()+static_cast<ptrdiff_t>(got)});sent+=got;reportProgress(progress,"network-upload",sent,size);}}checkCancelled(cancel);sendFrame(socket.get(),MessageType::UploadEnd,request);auto result=receiveFrame(socket.get());ensure(result.has_value(),"server closed after upload");if(result->type==MessageType::Error)throw NetError(errorFromFrame(*result));ensure(result->type==MessageType::UploadResult,"unexpected upload completion response");Reader done(result->payload);backupId=done.string();done.end();return true;}catch(const std::exception& e){error=e.what();return false;}
}

std::vector<RemoteBackupEntry> BackupClient::list(std::string& error) {
    try{Socket socket=connectTo(host_,port_);uint32_t request=0;ensure(authenticateClient(socket.get(),request,username_,password_,error),error);sendFrame(socket.get(),MessageType::ListRequest,++request);auto response=receiveFrame(socket.get());ensure(response.has_value(),"server closed during list request");if(response->type==MessageType::Error)throw NetError(errorFromFrame(*response));ensure(response->type==MessageType::ListResponse,"unexpected list response");Reader r(response->payload);uint32_t count=r.u32();ensure(count<=4096,"backup list count exceeds limit");std::vector<RemoteBackupEntry> entries;for(uint32_t i=0;i<count;++i){RemoteBackupEntry e;e.id=r.string();e.name=r.string();e.timestamp=r.u64();e.size=r.u64();e.digest=r.string();entries.push_back(std::move(e));}r.end();return entries;}catch(const std::exception& e){error=e.what();return {};}
}

bool BackupClient::download(const std::string& backupId,const std::string& outputPath,
                            std::string& error,ProgressCallback progress,
                            std::atomic_bool* cancel) {
    try{checkCancelled(cancel);ensure(validBackupId(backupId),"invalid backup identifier");struct stat outputState{};ensure(lstat(outputPath.c_str(),&outputState)!=0,"download output already exists");ensure(errno==ENOENT,"cannot inspect download output path");Socket socket=connectTo(host_,port_);uint32_t request=0;ensure(authenticateClient(socket.get(),request,username_,password_,error),error);checkCancelled(cancel);std::vector<uint8_t> payload;putString(payload,backupId);sendFrame(socket.get(),MessageType::DownloadRequest,++request,payload);auto start=receiveFrame(socket.get());ensure(start.has_value(),"server closed before download");if(start->type==MessageType::Error)throw NetError(errorFromFrame(*start));ensure(start->type==MessageType::DownloadStart&&start->requestId==request,"unexpected download response");Reader r(start->payload);uint64_t declaredSize=r.u64();auto digestBytes=r.bytes(32);r.end();std::array<uint8_t,32> declaredDigest{};std::copy(digestBytes.begin(),digestBytes.end(),declaredDigest.begin());fs::path output=fs::absolute(outputPath);fs::create_directories(output.parent_path());fs::path temp=output.string()+".tmp-"+randomId();struct Cleanup{fs::path p;~Cleanup(){std::error_code ec;fs::remove(p,ec);}}cleanup{temp};std::ofstream out(temp,std::ios::binary|std::ios::trunc);ensure(out.good(),"cannot create download output");chmod(temp.c_str(),0600);uint64_t received=0;while(true){checkCancelled(cancel);auto frame=receiveFrame(socket.get());ensure(frame.has_value(),"server disconnected during download");ensure(frame->requestId==request,"download request identifier changed");if(frame->type==MessageType::DownloadEnd)break;ensure(frame->type==MessageType::DownloadChunk,"unexpected download message");ensure(frame->payload.size()<=declaredSize-received,"download exceeds declared size");if(!frame->payload.empty())out.write(reinterpret_cast<const char*>(frame->payload.data()),static_cast<std::streamsize>(frame->payload.size()));ensure(out.good(),"cannot write download output");received+=frame->payload.size();reportProgress(progress,"network-download",received,declaredSize);}checkCancelled(cancel);out.close();ensure(received==declaredSize,"downloaded size mismatch");ensure(sha256File(temp.string())==declaredDigest,"downloaded SHA-256 mismatch");commitFileNoReplace(temp,output);return true;}catch(const std::exception& e){error=e.what();return false;}
}

} // namespace backup::network
