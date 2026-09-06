#include "net_internal.hpp"

namespace backup {
namespace network {
namespace detail {

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

void sendAll(int fd, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        ensure(n > 0, "network send failed");
        sent += static_cast<size_t>(n);
    }
}

bool receiveAll(int fd, uint8_t* data, size_t size, bool allowCleanEof) {
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


void sendFrame(int fd, MessageType type, uint32_t requestId, const std::vector<uint8_t>& payload) {
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

uint8_t Reader::u8() { need(1); return data_[offset_++]; }
uint16_t Reader::u16() { need(2); uint16_t v=(data_[offset_]<<8)|data_[offset_+1]; offset_+=2; return v; }
uint32_t Reader::u32() { need(4); uint32_t v=0; for(int i=0;i<4;++i)v=(v<<8)|data_[offset_++]; return v; }
uint64_t Reader::u64() { need(8); uint64_t v=0; for(int i=0;i<8;++i)v=(v<<8)|data_[offset_++]; return v; }
std::string Reader::string() {
    uint16_t n=u16(); need(n);
    std::string v(reinterpret_cast<const char*>(data_.data()+offset_),n);
    offset_+=n;
    ensure(v.find('\0')==std::string::npos,"network string contains NUL");
    return v;
}
std::vector<uint8_t> Reader::bytes(size_t n) {
    need(n);
    auto a=data_.begin()+static_cast<ptrdiff_t>(offset_);
    offset_+=n;
    return {a,a+static_cast<ptrdiff_t>(n)};
}
void Reader::end() const { ensure(offset_==data_.size(),"network message has trailing data"); }
void Reader::need(size_t n) const { ensure(offset_+n<=data_.size(),"truncated network message"); }

} // namespace detail
} // namespace network
} // namespace backup
