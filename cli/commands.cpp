#include "commands.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "backup/network.hpp"
#include "cli_args.hpp"


#include <termios.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace backup;

void progressReport(const ProgressEvent& event) {
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
    options.progress = progressReport;
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
    auto result = BackupEngine::create(args.positional[0], requiredOption(args.options,"-o"), backupOptions(args));
    std::cerr << '\n';
    if (!result.success) throw std::runtime_error(result.message);
    std::cout << "Backup complete: " << result.entryCount << " entries, " << result.outputBytes << " bytes\n";
    return 0;
}

int localRestore(const Arguments& args) {
    if (args.positional.size() != 1) throw std::runtime_error("restore requires one ARCHIVE");
    ArchiveInfo info = BackupEngine::inspect(args.positional[0]);
    RestoreOptions options; options.overwrite = hasFlag(args,"--overwrite"); options.progress = progressReport;
    if (info.encryption != EncryptionAlgorithm::None)
        options.password = secretFrom(args.options,"--key-file","Archive key: ");
    auto result = BackupEngine::restore(args.positional[0],requiredOption(args.options,"-d"),options);
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
    auto endpoint = parseServer(requiredOption(args.options,"--server"));
    std::string username = requiredOption(args.options,"--username");
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
    options.progress=progressReport;
    if(info.encryption!=EncryptionAlgorithm::None)
        options.password=secretFrom(args.options,"--key-file","Archive key: ");
    auto restored=BackupEngine::restore(temp,requiredOption(args.options,"-d"),options);
    std::cerr<<'\n';
    if(!restored.success)throw std::runtime_error(restored.message);
    std::cout<<"Remote restore complete: "<<restored.entryCount<<" entries\n";
    return 0;
}
