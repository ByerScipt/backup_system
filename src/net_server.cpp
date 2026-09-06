#include "net_internal.hpp"

namespace backup {
namespace network {
namespace detail {

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

} // namespace detail
} // namespace network
} // namespace backup

namespace backup {
namespace network {
using namespace detail;

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
} // namespace network
} // namespace backup
