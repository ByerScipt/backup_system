#include "arguments.hpp"

#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace
{

void validateArguments(const std::string& command, const Arguments& args)
{
    static const std::map<std::string, std::set<std::string>> allowed{
        {"backup",
         {"-o", "--pack", "--compress", "--encrypt", "--key-file",
          "--include"}},
        {"restore", {"-d", "--key-file", "--overwrite"}},
        {"watch",
         {"-o", "--pack", "--compress", "--encrypt", "--key-file", "--include",
          "--interval", "--retain", "--count", "--realtime"}},
        {"inspect", {}},
        {"user",
         {"--server", "--username", "--account-password-file", "--tls",
          "--tls-ca"}},
        {"remote-backup",
         {"--server", "--username", "--account-password-file", "--name",
          "--pack", "--compress", "--encrypt", "--key-file", "--include",
          "--incremental", "--tls", "--tls-ca"}},
        {"remote-list",
         {"--server", "--username", "--account-password-file", "--tls",
          "--tls-ca"}},
        {"remote-restore",
         {"--server", "--username", "--account-password-file", "-d",
          "--key-file", "--overwrite", "--tls", "--tls-ca"}}};
    auto found = allowed.find(command);
    if (found == allowed.end())
    {
        throw std::runtime_error("unknown command: " + command);
    }
    auto validate = [&](const std::string& option)
    {
        if (found->second.count(option) == 0)
        {
            throw std::runtime_error("unsupported option for " + command +
                                     ": " + option);
        }
    };
    for (const auto& option : args.options)
    {
        validate(option.first);
    }
    for (const auto& flag : args.flags)
    {
        validate(flag);
    }
    // Repeatable options are stored apart from single-valued ones, so they need
    // the same per-command check; otherwise --include would be accepted by
    // commands that never archive a source tree.
    for (const auto& repeated : args.repeated)
    {
        validate(repeated.first);
    }
    // Other commands validate their required operand in the command handler.
    if ((command == "user" || command == "remote-list") &&
        !args.positional.empty())
    {
        throw std::runtime_error(command + " accepts no positional arguments");
    }
}

void usage()
{
    std::cout << R"(backup-cli - streaming backup and restore system

Local:
  backup-cli backup SOURCE -o ARCHIVE --pack stream|index
      [--compress none|rle|huffman|lz77] [--encrypt none|chacha20|aes256]
      [--key-file FILE] [--include PATH]...
  backup-cli restore ARCHIVE -d DEST [--key-file FILE] [--overwrite]
  backup-cli inspect ARCHIVE
  backup-cli watch SOURCE -o DIRECTORY [--interval SECONDS] [--realtime]
      [--retain COUNT] [--count SNAPSHOTS] [local algorithm options]

Remote:
  backup-cli user register --server HOST:PORT --username NAME
      [--account-password-file FILE]
  backup-cli remote-backup SOURCE --server HOST:PORT --username NAME
      [--account-password-file FILE] [--name NAME] [--include PATH]...
      [--incremental] [local algorithm options]
  backup-cli remote-list --server HOST:PORT --username NAME
      [--account-password-file FILE]
  backup-cli remote-restore BACKUP_ID -d DEST --server HOST:PORT --username NAME
      [--account-password-file FILE] [--key-file FILE] [--overwrite]

If a required password file is omitted, the password is read interactively without echo.
Remote commands accept --tls (system trust) or --tls-ca FILE (explicit trust).
)";
}
} // namespace

int main(int argc, char* argv[])
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--version")
        {
            std::cout << "backup-cli " << BACKUP_SYSTEM_VERSION << "\n";
            return 0;
        }
        if (argc < 2 || std::string(argv[1]) == "--help" ||
            std::string(argv[1]) == "-h")
        {
            usage();
            return argc < 2 ? 1 : 0;
        }
        std::string command = argv[1];
        if (command == "user")
        {
            if (argc < 3 || std::string(argv[2]) != "register")
            {
                throw std::runtime_error("only 'user register' is supported");
            }
            Arguments args = parseArgs(argc, argv, 3);
            validateArguments(command, args);
            return registerUser(args);
        }
        Arguments args = parseArgs(argc, argv, 2);
        validateArguments(command, args);
        if (command == "backup")
        {
            return localBackup(args);
        }
        if (command == "restore")
        {
            return localRestore(args);
        }
        if (command == "watch")
        {
            return automaticBackup(args);
        }
        if (command == "inspect")
        {
            return inspectArchive(args);
        }
        if (command == "remote-backup")
        {
            return remoteBackup(args);
        }
        if (command == "remote-list")
        {
            return remoteList(args);
        }
        if (command == "remote-restore")
        {
            return remoteRestore(args);
        }
        throw std::runtime_error("unknown command: " + command);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
