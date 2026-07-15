#include "backup/core.hpp"
#include "backup/network.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace backup;

namespace {

bool gSocketFixtureAvailable = false;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct TempDirectory {
    fs::path path;
    TempDirectory() {
        std::string pattern = (fs::temp_directory_path()/"backup-tests-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(),pattern.end());buffer.push_back('\0');
        char* created=mkdtemp(buffer.data());check(created!=nullptr,"mkdtemp failed");path=created;
    }
    ~TempDirectory(){std::error_code ec;fs::remove_all(path,ec);}
};

void writeBytes(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);check(out.good(),"cannot write fixture");
    if(!data.empty())out.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));
}

std::vector<uint8_t> readBytes(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);check(in.good(),"cannot read file");
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}

void makeSocketNode(const fs::path& path) {
    int fd=::socket(AF_UNIX,SOCK_STREAM,0);check(fd>=0,"socket fixture failed");sockaddr_un address{};address.sun_family=AF_UNIX;check(path.string().size()<sizeof(address.sun_path),"socket fixture path too long");std::strncpy(address.sun_path,path.c_str(),sizeof(address.sun_path)-1);int result=::bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address));int saved=errno;::close(fd);
    if(result==0){gSocketFixtureAvailable=true;return;}
    if(saved==EPERM||saved==EACCES){std::cout<<"SKIP: Unix socket nodes are blocked by this sandbox\n";return;}
    check(false,"socket fixture bind failed for "+path.string()+": "+std::strerror(saved));
}

void createFixture(const fs::path& root) {
    fs::create_directories(root/"nested"/"emptydir");fs::create_directories(root/"中文目录");
    writeBytes(root/"empty.bin",{});writeBytes(root/"hello.txt",{'h','e','l','l','o','\n'});
    std::vector<uint8_t> all(256);for(int i=0;i<256;++i)all[i]=static_cast<uint8_t>(i);writeBytes(root/"all-bytes.bin",all);
    std::vector<uint8_t> rleBoundary;rleBoundary.insert(rleBoundary.end(),127,'A');rleBoundary.insert(rleBoundary.end(),128,'B');rleBoundary.insert(rleBoundary.end(),2,'C');rleBoundary.insert(rleBoundary.end(),3,'D');rleBoundary.insert(rleBoundary.end(),129,'E');writeBytes(root/"rle-boundaries.bin",rleBoundary);
    writeBytes(root/"nested"/"single-symbol.bin",std::vector<uint8_t>(4096,0x41));
    writeBytes(root/"中文目录"/"数据.txt",{'U','T','F','-','8','\n'});
    chmod((root/"hello.txt").c_str(),0600);
    check(::symlink("../hello.txt",(root/"nested"/"hello-link").c_str())==0,"symlink fixture failed");
    check(::mkfifo((root/"named-pipe").c_str(),0640)==0,"FIFO fixture failed");
    makeSocketNode(root/"unix-socket");
}

void compareFixture(const fs::path& source, const fs::path& restored) {
    check(readBytes(source/"empty.bin")==readBytes(restored/"empty.bin"),"empty file mismatch");
    check(readBytes(source/"hello.txt")==readBytes(restored/"hello.txt"),"text file mismatch");
    check(readBytes(source/"all-bytes.bin")==readBytes(restored/"all-bytes.bin"),"binary file mismatch");
    check(readBytes(source/"rle-boundaries.bin")==readBytes(restored/"rle-boundaries.bin"),"RLE boundary file mismatch");
    check(readBytes(source/"nested"/"single-symbol.bin")==readBytes(restored/"nested"/"single-symbol.bin"),"repeat file mismatch");
    check(fs::is_directory(restored/"nested"/"emptydir"),"empty directory missing");
    check(fs::is_symlink(restored/"nested"/"hello-link"),"symbolic link missing");
    check(fs::read_symlink(restored/"nested"/"hello-link")==fs::path("../hello.txt"),"symbolic link target mismatch");
    struct stat st{};check(lstat((restored/"named-pipe").c_str(),&st)==0&&S_ISFIFO(st.st_mode),"FIFO missing");
    if(gSocketFixtureAvailable)check(lstat((restored/"unix-socket").c_str(),&st)==0&&S_ISSOCK(st.st_mode),"Unix socket node missing");
    check(lstat((restored/"hello.txt").c_str(),&st)==0&&(st.st_mode&0777)==0600,"file mode mismatch");
}

void testSha256() {
    check(hexDigest(sha256(std::string("abc")))=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA-256 known vector failed");
}

struct BuiltArchive { fs::path path; PackAlgorithm pack; CompressionAlgorithm compression; EncryptionAlgorithm encryption; };

std::vector<BuiltArchive> testAllCombinations(const fs::path& workspace, const fs::path& source) {
    std::vector<BuiltArchive> archives;int number=0;
    for(auto pack:{PackAlgorithm::Stream,PackAlgorithm::Index})
        for(auto compression:{CompressionAlgorithm::None,CompressionAlgorithm::Rle,CompressionAlgorithm::Huffman})
            for(auto encryption:{EncryptionAlgorithm::None,EncryptionAlgorithm::Xor,EncryptionAlgorithm::Vigenere}){
                fs::path archive=workspace/("combo-"+std::to_string(number)+".bak");BackupOptions options;options.pack=pack;options.compression=compression;options.encryption=encryption;if(encryption!=EncryptionAlgorithm::None)options.password="correct horse battery staple";auto created=BackupEngine::create(source.string(),archive.string(),options);check(created.success,"combination backup failed: "+created.message);auto info=BackupEngine::inspect(archive.string());check(info.pack==pack&&info.compression==compression&&info.encryption==encryption,"inspect algorithm mismatch");fs::path destination=workspace/("restore-"+std::to_string(number));RestoreOptions restore;if(encryption!=EncryptionAlgorithm::None)restore.password=options.password;auto restored=BackupEngine::restore(archive.string(),destination.string(),restore);check(restored.success,"combination restore failed: "+restored.message);compareFixture(source,destination/source.filename());archives.push_back({archive,pack,compression,encryption});++number;
            }
    check(archives.size()==18,"not all 18 algorithm combinations ran");return archives;
}

void testFailureModes(const fs::path& workspace, const fs::path& source,
                      const std::vector<BuiltArchive>& archives) {
    auto encrypted=std::find_if(archives.begin(),archives.end(),[](const auto& a){return a.encryption==EncryptionAlgorithm::Xor&&a.compression==CompressionAlgorithm::Huffman;});check(encrypted!=archives.end(),"encrypted fixture missing");RestoreOptions wrong;wrong.password="wrong password";auto wrongResult=BackupEngine::restore(encrypted->path.string(),(workspace/"wrong-password").string(),wrong);check(!wrongResult.success,"wrong password was accepted");check(!fs::exists(workspace/"wrong-password"/source.filename()),"wrong password created output tree");

    auto plain=std::find_if(archives.begin(),archives.end(),[](const auto& a){return a.encryption==EncryptionAlgorithm::None&&a.compression==CompressionAlgorithm::None&&a.pack==PackAlgorithm::Stream;});check(plain!=archives.end(),"plain fixture missing");
    fs::path conflict=workspace/"conflict";RestoreOptions first;auto one=BackupEngine::restore(plain->path.string(),conflict.string(),first);check(one.success,"initial conflict fixture restore failed");auto two=BackupEngine::restore(plain->path.string(),conflict.string(),first);check(!two.success,"restore conflict was not rejected");RestoreOptions overwrite;overwrite.overwrite=true;auto three=BackupEngine::restore(plain->path.string(),conflict.string(),overwrite);check(three.success,"explicit overwrite failed: "+three.message);
    auto preview=BackupEngine::preview(plain->path.string(),conflict.string());check(preview.entries.size()>=10,"restore preview omitted entries");check(!preview.conflicts.empty(),"restore preview did not report conflicts");

    fs::path corrupt=workspace/"corrupt.bak";fs::copy_file(plain->path,corrupt);auto bytes=readBytes(corrupt);bytes.back()^=0x5a;writeBytes(corrupt,bytes);bool rejected=false;try{static_cast<void>(BackupEngine::inspect(corrupt.string()));}catch(...){rejected=true;}check(rejected,"corrupt archive passed inspect");
    fs::path truncated=workspace/"truncated.bak";writeBytes(truncated,std::vector<uint8_t>(bytes.begin(),bytes.begin()+50));rejected=false;try{static_cast<void>(BackupEngine::inspect(truncated.string()));}catch(...){rejected=true;}check(rejected,"truncated archive passed inspect");

    fs::path invalidAlgorithm=workspace/"invalid-algorithm.bak";auto invalidBytes=readBytes(plain->path);invalidBytes[12]=9;writeBytes(invalidAlgorithm,invalidBytes);rejected=false;try{static_cast<void>(BackupEngine::inspect(invalidAlgorithm.string()));}catch(...){rejected=true;}check(rejected,"invalid algorithm identifier passed inspect");

    fs::path inside=source/"inside.bak";BackupOptions options;auto insideResult=BackupEngine::create(source.string(),inside.string(),options);check(!insideResult.success,"archive inside source was accepted");
    fs::path brokenOutput=workspace/"broken-output.bak";check(::symlink("missing-target",brokenOutput.c_str())==0,"broken output symlink fixture failed");auto brokenResult=BackupEngine::create(source.string(),brokenOutput.string(),options);check(!brokenResult.success,"broken output symlink was overwritten");check(fs::is_symlink(brokenOutput),"broken output symlink was not preserved");

    fs::path traversal=workspace/"traversal.bak";auto malicious=readBytes(plain->path);std::string root=source.filename().string();check(root.size()==7,"fixture root name must be seven bytes for traversal test");auto begin=std::search(malicious.begin()+112,malicious.end(),root.begin(),root.end());check(begin!=malicious.end(),"could not locate root name in test archive");std::string replacement="../evil";std::copy(replacement.begin(),replacement.end(),begin);std::vector<uint8_t> payload(malicious.begin()+112,malicious.end());auto digest=sha256(payload);std::copy(digest.begin(),digest.end(),malicious.begin()+48);std::copy(digest.begin(),digest.end(),malicious.begin()+80);writeBytes(traversal,malicious);auto pathResult=BackupEngine::restore(traversal.string(),(workspace/"path-target").string(),{});check(!pathResult.success,"path traversal archive was accepted");check(!fs::exists(workspace/"evil"),"path traversal escaped destination");

    fs::path duplicate=workspace/"duplicate.bak";auto duplicateBytes=readBytes(plain->path);std::string original=root+"/hello.txt",replacementPath=root+"/empty.bin";check(original.size()==replacementPath.size(),"duplicate-path fixture lengths differ");auto duplicateAt=std::search(duplicateBytes.begin()+112,duplicateBytes.end(),original.begin(),original.end());check(duplicateAt!=duplicateBytes.end(),"could not locate duplicate-path fixture");std::copy(replacementPath.begin(),replacementPath.end(),duplicateAt);payload.assign(duplicateBytes.begin()+112,duplicateBytes.end());digest=sha256(payload);std::copy(digest.begin(),digest.end(),duplicateBytes.begin()+48);std::copy(digest.begin(),digest.end(),duplicateBytes.begin()+80);writeBytes(duplicate,duplicateBytes);auto duplicateResult=BackupEngine::restore(duplicate.string(),(workspace/"duplicate-target").string(),{});check(!duplicateResult.success,"duplicate archive path was accepted");

    fs::path oversized=workspace/"oversized-length.bak";auto oversizedBytes=readBytes(plain->path);auto rootAt=std::search(oversizedBytes.begin()+112,oversizedBytes.end(),root.begin(),root.end());check(rootAt!=oversizedBytes.end()&&rootAt-oversizedBytes.begin()>=4,"could not locate length field");std::fill(rootAt-4,rootAt,0xff);payload.assign(oversizedBytes.begin()+112,oversizedBytes.end());digest=sha256(payload);std::copy(digest.begin(),digest.end(),oversizedBytes.begin()+48);std::copy(digest.begin(),digest.end(),oversizedBytes.begin()+80);writeBytes(oversized,oversizedBytes);auto oversizedResult=BackupEngine::restore(oversized.string(),(workspace/"oversized-target").string(),{});check(!oversizedResult.success,"oversized path length was accepted");

    fs::path symlinkEscape=workspace/"symlink-descendant.bak";auto symlinkBytes=readBytes(plain->path);std::string regularPath=root+"/nested/single-symbol.bin",childPath=root+"/nested/hello-link/escape";check(regularPath.size()==childPath.size(),"symlink-descendant fixture lengths differ");auto regularAt=std::search(symlinkBytes.begin()+112,symlinkBytes.end(),regularPath.begin(),regularPath.end());check(regularAt!=symlinkBytes.end(),"could not locate symlink-descendant path");std::copy(childPath.begin(),childPath.end(),regularAt);std::string safeTarget="../hello.txt",escapeTarget="../../../out";check(safeTarget.size()==escapeTarget.size(),"symlink target fixture lengths differ");auto targetAt=std::search(symlinkBytes.begin()+112,symlinkBytes.end(),safeTarget.begin(),safeTarget.end());check(targetAt!=symlinkBytes.end(),"could not locate symlink target fixture");std::copy(escapeTarget.begin(),escapeTarget.end(),targetAt);payload.assign(symlinkBytes.begin()+112,symlinkBytes.end());digest=sha256(payload);std::copy(digest.begin(),digest.end(),symlinkBytes.begin()+48);std::copy(digest.begin(),digest.end(),symlinkBytes.begin()+80);writeBytes(symlinkEscape,symlinkBytes);auto symlinkResult=BackupEngine::restore(symlinkEscape.string(),(workspace/"symlink-target").string(),{});check(!symlinkResult.success,"archive entry below a symlink was accepted");check(!fs::exists(workspace/"out"),"archive symlink escaped the destination");

    auto huffman=std::find_if(archives.begin(),archives.end(),[](const auto& a){return a.encryption==EncryptionAlgorithm::None&&a.compression==CompressionAlgorithm::Huffman&&a.pack==PackAlgorithm::Stream;});check(huffman!=archives.end(),"Huffman fixture missing");fs::path huffmanTrailing=workspace/"huffman-trailing.bak";auto huffmanBytes=readBytes(huffman->path);uint64_t encodedSize=0;for(int i=0;i<8;++i)encodedSize|=static_cast<uint64_t>(huffmanBytes[24+i])<<(i*8);huffmanBytes.push_back(0);++encodedSize;for(int i=0;i<8;++i)huffmanBytes[24+i]=static_cast<uint8_t>(encodedSize>>(i*8));payload.assign(huffmanBytes.begin()+112,huffmanBytes.end());digest=sha256(payload);std::copy(digest.begin(),digest.end(),huffmanBytes.begin()+80);writeBytes(huffmanTrailing,huffmanBytes);auto trailingResult=BackupEngine::restore(huffmanTrailing.string(),(workspace/"huffman-trailing-target").string(),{});check(!trailingResult.success,"Huffman trailing data was accepted");
}

uint16_t reservePort() {
    int fd=::socket(AF_INET,SOCK_STREAM,0);
    if(fd<0&&(errno==EPERM||errno==EACCES)){std::cout<<"SKIP: TCP sockets are blocked by this sandbox\n";return 0;}
    check(fd>=0,"port reservation socket failed");sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=0;int bound=::bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address));int saved=errno;
    if(bound!=0&&(saved==EPERM||saved==EACCES)){::close(fd);std::cout<<"SKIP: TCP bind is blocked by this sandbox\n";return 0;}
    check(bound==0,"port reservation bind failed");socklen_t size=sizeof(address);check(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&size)==0,"getsockname failed");uint16_t port=ntohs(address.sin_port);::close(fd);return port;
}

void testNetwork(const fs::path& workspace, const fs::path& archive) {
    uint16_t port=reservePort();if(port==0)return;network::ServerConfig config;config.port=port;config.storagePath=(workspace/"server-data").string();config.timeoutSeconds=3;network::BackupServer server(config);std::thread thread([&](){check(server.run(),"server run failed");});std::this_thread::sleep_for(std::chrono::milliseconds(200));
    try{
        std::string error;network::BackupClient alice("127.0.0.1",port,"alice","alice-password");check(alice.registerUser(error),"Alice registration failed: "+error);error.clear();check(!alice.registerUser(error),"duplicate registration was accepted");error.clear();std::string id;check(alice.upload(archive.string(),"integration backup",id,error),"network upload failed: "+error);check(id.size()==32,"server returned invalid backup ID");error.clear();auto entries=alice.list(error);check(error.empty()&&entries.size()==1&&entries[0].id==id,"network list mismatch: "+error);fs::path downloaded=workspace/"downloaded.bak";check(alice.download(id,downloaded.string(),error),"network download failed: "+error);check(sha256File(downloaded.string())==sha256File(archive.string()),"network roundtrip checksum mismatch");
        network::BackupClient wrong("127.0.0.1",port,"alice","wrong");error.clear();auto denied=wrong.list(error);check(denied.empty()&&!error.empty(),"wrong account password was accepted");
        network::BackupClient bob("127.0.0.1",port,"bob","bob-password");error.clear();check(bob.registerUser(error),"Bob registration failed: "+error);error.clear();auto bobEntries=bob.list(error);check(error.empty()&&bobEntries.empty(),"cross-user list isolation failed");error.clear();check(!bob.download(id,(workspace/"bob-download.bak").string(),error),"cross-user download was accepted");
    }catch(...){server.stop();thread.join();throw;}
    server.stop();thread.join();
    network::BackupServer restarted(config);std::thread restartThread([&](){check(restarted.run(),"restarted server run failed");});std::this_thread::sleep_for(std::chrono::milliseconds(200));
    try{std::string error;network::BackupClient alice("127.0.0.1",port,"alice","alice-password");auto entries=alice.list(error);check(error.empty()&&entries.size()==1,"server restart did not preserve backup history: "+error);}catch(...){restarted.stop();restartThread.join();throw;}
    restarted.stop();restartThread.join();
}

} // namespace

int main() {
    try{
        std::cout<<"[1/4] SHA-256\n";testSha256();TempDirectory temp;fs::path source=temp.path/"testsrc";createFixture(source);
        std::cout<<"[2/4] 18 algorithm combinations\n";auto archives=testAllCombinations(temp.path,source);
        std::cout<<"[3/4] corruption, conflicts, passwords, traversal\n";testFailureModes(temp.path,source,archives);
        std::cout<<"[4/4] account-isolated network roundtrip\n";testNetwork(temp.path,archives.front().path);
        std::cout<<"All backup-system tests passed.\n";return 0;
    }catch(const std::exception& error){std::cerr<<"TEST FAILURE: "<<error.what()<<"\n";return 1;}
}
