#include "network.hpp"

#include <charconv>
#include <csignal>
#include <iostream>
#include <map>
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
        std::map<std::string, std::string> arguments;
        bool showHelp = false;
        for (int i = 1; i < argc; ++i)
        {
            std::string option = argv[i];
            if (option == "--help" || option == "-h")
            {
                showHelp = true;
                continue;
            }
            if ((option != "--config" && option != "--bind" &&
                 option != "--port" && option != "--storage" &&
                 option != "--max-connections" && option != "--tls-cert" &&
                 option != "--tls-key") ||
                i + 1 == argc)
            {
                throw std::runtime_error("unknown or incomplete option: " +
                                         option);
            }
            if (!arguments.emplace(option, argv[++i]).second)
            {
                throw std::runtime_error("duplicate option: " + option);
            }
        }
        auto config =
            arguments.count("--config")
                ? backup::network::ServerConfig::load(arguments.at("--config"))
                : backup::network::ServerConfig{};
        // Load defaults once; explicit options always take precedence.
        for (const auto& [option, value] : arguments)
        {
            if (option == "--bind")
            {
                config.bindAddress = value;
                if (!backup::network::isIPv4Address(config.bindAddress))
                {
                    throw std::runtime_error("invalid value for " + option +
                                             ": " + config.bindAddress);
                }
            }
            else if (option == "--port")
            {
                config.port =
                    static_cast<uint16_t>(numericOption(value, 65535, option));
            }
            else if (option == "--storage")
            {
                if (value.empty())
                {
                    throw std::runtime_error("empty value for " + option);
                }
                config.storagePath = value;
            }
            else if (option == "--max-connections")
            {
                config.maxConnections = numericOption(value, 1024, option);
            }
            else if (option == "--tls-cert")
            {
                if (value.empty())
                {
                    throw std::runtime_error("empty value for --tls-cert");
                }
                config.tlsCertificate = value;
            }
            else if (option == "--tls-key")
            {
                if (value.empty())
                {
                    throw std::runtime_error("empty value for --tls-key");
                }
                config.tlsPrivateKey = value;
            }
        }
        if (showHelp)
        {
            std::cout << "Usage: backup-server [--config FILE] [--bind "
                         "ADDRESS] [--port PORT]\n"
                         "                     [--storage DIR] "
                         "[--max-connections N]\n";
            std::cout
                << "                     [--tls-cert PEM --tls-key PEM]\n";
            return 0;
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
