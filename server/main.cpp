#include "backup/network.hpp"

#include <charconv>
#include <csignal>
#include <iostream>
#include <string>

namespace
{
backup::network::BackupServer* gServer = nullptr;
uint32_t numericOption(const std::string& value, uint32_t maximum,
                       const std::string& option)
{
    uint32_t number = 0;
    auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        number == 0 || number > maximum)
    {
        throw std::runtime_error("invalid value for " + option + ": " + value);
    }
    return number;
}
void stopHandler(int)
{
    if (gServer)
    {
        gServer->stop();
    }
}
} // namespace

int main(int argc, char* argv[])
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--version")
        {
            std::cout << "backup-server " << BACKUP_SYSTEM_VERSION << "\n";
            return 0;
        }
        backup::network::ServerConfig config;
        for (int i = 1; i < argc; ++i)
        {
            std::string option = argv[i];
            if (option == "--config" && i + 1 < argc)
            {
                config = backup::network::ServerConfig::load(argv[++i]);
            }
            else if (option == "--port" && i + 1 < argc)
            {
                config.port = static_cast<uint16_t>(
                    numericOption(argv[++i], 65535, option));
            }
            else if (option == "--storage" && i + 1 < argc)
            {
                config.storagePath = argv[++i];
            }
            else if (option == "--max-connections" && i + 1 < argc)
            {
                config.maxConnections = numericOption(argv[++i], 1024, option);
            }
            else if (option == "--help" || option == "-h")
            {
                std::cout << "Usage: backup-server [--config FILE] [--port "
                             "PORT] [--storage DIR] "
                             "[--max-connections N]\n";
                return 0;
            }
            else
            {
                throw std::runtime_error("unknown or incomplete option: " +
                                         option);
            }
        }
        backup::network::BackupServer server(config);
        gServer = &server;
        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);
        bool ok = server.run();
        gServer = nullptr;
        return ok ? 0 : 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
