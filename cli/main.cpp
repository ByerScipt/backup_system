#include "backup/core.hpp"
#include "backup/network.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <termios.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace backup;

namespace {

void usage() {
    std::cout << R"(backup-cli - streaming backup and restore system

Local:
  backup-cli backup SOURCE -o ARCHIVE --pack stream|index
      [--compress none|rle|huffman] [--encrypt none|xor|vigenere]
      [--key-file FILE]
  backup-cli restore ARCHIVE -d DEST [--key-file FILE] [--overwrite]
  backup-cli inspect ARCHIVE

Remote:
  backup-cli user register --server HOST:PORT --username NAME
      [--account-password-file FILE]
  backup-cli remote-backup SOURCE --server HOST:PORT --username NAME
      [--account-password-file FILE] [--name NAME] [local algorithm options]
  backup-cli remote-list --server HOST:PORT --username NAME
      [--account-password-file FILE]
  backup-cli remote-restore BACKUP_ID -d DEST --server HOST:PORT --username NAME
      [--account-password-file FILE] [--key-file FILE] [--overwrite]

If a required password file is omitted, the password is read interactively without echo.
)";
}

std::string readFileSecret(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open password file: " + path);
    std::string value((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    if (value.empty()) throw std::runtime_error("password file is empty: " + path);
    return value;
}

std::string promptSecret(const std::string& prompt) {
    std::cerr << prompt;
    termios oldState{};
    bool hidden = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &oldState) == 0;
    if (hidden) {
        termios state = oldState;
        state.c_lflag &= static_cast<tcflag_t>(~ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &state);
    }
    std::string value;
    std::getline(std::cin, value);
    if (hidden) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldState);
        std::cerr << '\n';
    }
    if (value.empty()) throw std::runtime_error("password cannot be empty");
    return value;
}

std::string secretFrom(const std::map<std::string,std::string>& options,
                       const std::string& key, const std::string& prompt) {
    auto it = options.find(key);
    return it == options.end() ? promptSecret(prompt) : readFileSecret(it->second);
}

struct Arguments {
    std::vector<std::string> positional;
    std::map<std::string,std::string> options;
    std::vector<std::string> flags;
};

Arguments parse(int argc, char* argv[], int start) {
    Arguments result;
    for (int i = start; i < argc; ++i) {
        std::string value = argv[i];
        if (value == "--overwrite") result.flags.push_back(value);
        else if (value == "-o" || value == "-d" || value == "--pack" || value == "--compress" ||
                 value == "--encrypt" || value == "--key-file" || value == "--server" ||
                 value == "--username" || value == "--account-password-file" || value == "--name") {
            if (i + 1 >= argc) throw std::runtime_error("missing value for option: " + value);
            result.options[value] = argv[++i];
        } else if (!value.empty() && value[0] == '-') {
            throw std::runtime_error("unknown option: " + value);
        } else result.positional.push_back(value);
    }
    return result;
}

std::string required(const std::map<std::string,std::string>& options, const std::string& name) {
    auto it = options.find(name);
    if (it == options.end() || it->second.empty()) throw std::runtime_error("required option is missing: " + name);
    return it->second;
}

bool hasFlag(const Arguments& args, const std::string& flag) {
    return std::find(args.flags.begin(), args.flags.end(), flag) != args.flags.end();
}

std::pair<std::string,uint16_t> parseServer(const std::string& address) {
    auto colon = address.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == address.size())
        throw std::runtime_error("server must use HOST:PORT format");
    unsigned long port = std::stoul(address.substr(colon + 1));
    if (port == 0 || port > 65535) throw std::runtime_error("server port is out of range");
    std::string host = address.substr(0, colon);
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    return {host, static_cast<uint16_t>(port)};
}

void progress(const ProgressEvent& event) {
    std::cerr << "\r[" << event.stage << "] ";
    if (event.total) std::cerr << event.completed << "/" << event.total;
    else std::cerr << event.completed;
    if (!event.detail.empty()) std::cerr << " " << event.detail;
    std::cerr << "        " << std::flush;
}

BackupOptions backupOptions(const Arguments& args) {
    BackupOptions options;
    auto pack = args.options.find("--pack");
    auto compression = args.options.find("--compress");
    auto encryption = args.options.find("--encrypt");
    options.pack = parsePackAlgorithm(pack == args.options.end() ? "stream" : pack->second);
    options.compression = parseCompressionAlgorithm(compression == args.options.end() ? "none" : compression->second);
    options.encryption = parseEncryptionAlgorithm(encryption == args.options.end() ? "none" : encryption->second);
    if (options.encryption != EncryptionAlgorithm::None)
        options.password = secretFrom(args.options, "--key-file", "Archive key: ");
    options.progress = progress;
    return options;
}

std::string temporaryArchive() {
    std::string pattern = (fs::temp_directory_path() / "backup-cli-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end()); buffer.push_back('\0');
    int fd = mkstemp(buffer.data());
    if (fd < 0) throw std::runtime_error("cannot allocate temporary archive path");
    close(fd); unlink(buffer.data());
    return buffer.data();
}

struct RemoveOnExit {
    fs::path path;
    ~RemoveOnExit() { std::error_code ec; fs::remove(path, ec); }
};

int localBackup(const Arguments& args) {
    if (args.positional.size() != 1) throw std::runtime_error("backup requires one SOURCE directory");
    auto result = BackupEngine::create(args.positional[0], required(args.options,"-o"), backupOptions(args));
    std::cerr << '\n';
    if (!result.success) throw std::runtime_error(result.message);
    std::cout << "Backup complete: " << result.entryCount << " entries, " << result.outputBytes << " bytes\n";
    return 0;
}

int localRestore(const Arguments& args) {
    if (args.positional.size() != 1) throw std::runtime_error("restore requires one ARCHIVE");
    ArchiveInfo info = BackupEngine::inspect(args.positional[0]);
    RestoreOptions options; options.overwrite = hasFlag(args,"--overwrite"); options.progress = progress;
    if (info.encryption != EncryptionAlgorithm::None)
        options.password = secretFrom(args.options,"--key-file","Archive key: ");
    auto result = BackupEngine::restore(args.positional[0],required(args.options,"-d"),options);
    std::cerr << '\n';
    if (!result.success) throw std::runtime_error(result.message);
    std::cout << "Restore complete: " << result.entryCount << " entries, " << result.outputBytes << " file bytes\n";
    return 0;
}

int inspectArchive(const Arguments& args) {
    if (args.positional.size() != 1) throw std::runtime_error("inspect requires one ARCHIVE");
    auto info = BackupEngine::inspect(args.positional[0]);
    std::cout << "Format: BKP2 version " << info.version << "\n"
              << "Pack: " << toString(info.pack) << "\n"
              << "Compression: " << toString(info.compression) << "\n"
              << "Encryption: " << toString(info.encryption) << "\n"
              << "Packed bytes: " << info.packedSize << "\n"
              << "Encoded bytes: " << info.encodedSize << "\n"
              << "Packed SHA-256: " << hexDigest(info.packedDigest) << "\n"
              << "Payload SHA-256: " << hexDigest(info.encodedDigest) << "\n";
    return 0;
}

network::BackupClient remoteClient(const Arguments& args) {
    auto endpoint = parseServer(required(args.options,"--server"));
    std::string username = required(args.options,"--username");
    std::string accountPassword = secretFrom(args.options,"--account-password-file","Account password: ");
    return {endpoint.first,endpoint.second,username,accountPassword};
}

int registerUser(const Arguments& args) {
    auto client = remoteClient(args); std::string error;
    if (!client.registerUser(error)) throw std::runtime_error(error);
    std::cout << "User registered successfully.\n"; return 0;
}

int remoteBackup(const Arguments& args) {
    if (args.positional.size()!=1)throw std::runtime_error("remote-backup requires one SOURCE directory");
    std::string temp=temporaryArchive();RemoveOnExit cleanup{temp};auto local=BackupEngine::create(args.positional[0],temp,backupOptions(args));std::cerr<<'\n';if(!local.success)throw std::runtime_error(local.message);
    auto client=remoteClient(args);std::string id,error;auto name=args.options.find("--name");if(!client.upload(temp,name==args.options.end()?"":name->second,id,error))throw std::runtime_error(error);std::cout<<"Remote backup complete. ID: "<<id<<"\n";return 0;
}

int remoteList(const Arguments& args) {
    auto client=remoteClient(args);std::string error;auto entries=client.list(error);if(!error.empty())throw std::runtime_error(error);std::cout<<"Remote backups ("<<entries.size()<<"):\n";for(const auto& e:entries)std::cout<<e.id<<"\t"<<e.name<<"\t"<<e.size<<" bytes\t"<<e.timestamp<<"\n";return 0;
}

int remoteRestore(const Arguments& args) {
    if(args.positional.size()!=1)
        throw std::runtime_error("remote-restore requires one BACKUP_ID");
    std::string temp=temporaryArchive();
    RemoveOnExit cleanup{temp};
    auto client=remoteClient(args);
    std::string error;
    if(!client.download(args.positional[0],temp,error))throw std::runtime_error(error);
    ArchiveInfo info=BackupEngine::inspect(temp);
    RestoreOptions options;
    options.overwrite=hasFlag(args,"--overwrite");
    options.progress=progress;
    if(info.encryption!=EncryptionAlgorithm::None)
        options.password=secretFrom(args.options,"--key-file","Archive key: ");
    auto restored=BackupEngine::restore(temp,required(args.options,"-d"),options);
    std::cerr<<'\n';
    if(!restored.success)throw std::runtime_error(restored.message);
    std::cout<<"Remote restore complete: "<<restored.entryCount<<" entries\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") { usage(); return argc < 2 ? 1 : 0; }
        std::string command=argv[1];
        if(command=="user"){
            if(argc<3||std::string(argv[2])!="register")throw std::runtime_error("only 'user register' is supported");
            return registerUser(parse(argc,argv,3));
        }
        Arguments args=parse(argc,argv,2);
        if(command=="backup")return localBackup(args);
        if(command=="restore")return localRestore(args);
        if(command=="inspect")return inspectArchive(args);
        if(command=="remote-backup")return remoteBackup(args);
        if(command=="remote-list")return remoteList(args);
        if(command=="remote-restore")return remoteRestore(args);
        throw std::runtime_error("unknown command: "+command);
    } catch(const std::exception& error){std::cerr<<"Error: "<<error.what()<<"\n";return 1;}
}
