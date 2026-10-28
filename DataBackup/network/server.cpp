#include "internal.hpp"
#include <charconv>
#include <limits>

namespace backup::network
{
using namespace detail;

bool isIPv4Address(const std::string& value)
{
    in_addr parsed{};
    return value.find('\0') == std::string::npos &&
           ::inet_pton(AF_INET, value.c_str(), &parsed) == 1;
}

ServerConfig ServerConfig::load(const std::string& path)
{
    ServerConfig config;
    std::ifstream in(path);
    if (!in)
    {
        throw std::runtime_error("cannot open server config: " + path);
    }
    std::string line;
    auto trim = [](std::string value)
    {
        auto first = value.find_first_not_of(" \t\r\n");
        auto last = value.find_last_not_of(" \t\r\n");
        return first == std::string::npos
                   ? std::string()
                   : value.substr(first, last - first + 1);
    };
    auto parseNumber = [](const std::string& value, uint32_t maximum)
    {
        uint32_t number = 0;
        auto result =
            std::from_chars(value.data(), value.data() + value.size(), number);
        if (result.ec != std::errc{} ||
            result.ptr != value.data() + value.size() || number == 0 ||
            number > maximum)
        {
            throw std::runtime_error("invalid numeric server setting: " +
                                     value);
        }
        return number;
    };
    while (std::getline(in, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
        {
            continue;
        }
        auto equals = line.find('=');
        ensure(equals != std::string::npos,
               "server setting must use key = value: " + line);
        std::string key = trim(line.substr(0, equals)),
                    value = trim(line.substr(equals + 1));
        if (key == "bind_address")
        {
            ensure(isIPv4Address(value),
                   "bind_address must be an IPv4 literal: " + value);
            config.bindAddress = value;
        }
        else if (key == "port")
        {
            config.port = static_cast<uint16_t>(parseNumber(value, 65535));
        }
        else if (key == "storage_path")
        {
            ensure(!value.empty(), "storage_path must not be empty");
            config.storagePath = value;
        }
        else if (key == "tls_certificate")
        {
            config.tlsCertificate = value;
        }
        else if (key == "tls_private_key")
        {
            config.tlsPrivateKey = value;
        }
        else if (key == "max_connections")
        {
            config.maxConnections = parseNumber(value, 1024);
        }
        else if (key == "timeout_seconds")
        {
            config.timeoutSeconds =
                parseNumber(value, std::numeric_limits<uint32_t>::max());
        }
        else
        {
            throw NetError("unknown server setting: " + key);
        }
    }
    ensure(in.eof() && !in.bad(),
           "cannot read complete server config: " + path);
    return config;
}

BackupServer::BackupServer(ServerConfig config) : config_(std::move(config)) {}

bool BackupServer::run()
{
    stopRequested_ = false;
    struct Session
    {
        std::thread worker;
        std::shared_ptr<std::atomic_bool> finished;
    };
    std::vector<Session> sessions;
    auto reap = [&](bool all)
    {
        for (auto it = sessions.begin(); it != sessions.end();)
        {
            if (all || it->finished->load())
            {
                if (it->worker.joinable())
                {
                    it->worker.join();
                }
                it = sessions.erase(it);
            }
            else
            {
                ++it;
            }
        }
    };
    bool success = false;
    Socket listener;
    try
    {
        ensure(config_.port != 0 && config_.maxConnections > 0 &&
                   config_.maxConnections <= 1024 &&
                   config_.timeoutSeconds > 0 && !config_.storagePath.empty(),
               "invalid server configuration");
        sessions.reserve(config_.maxConnections);
        const auto tls = serverTlsContext(config_);
        fs::create_directories(config_.storagePath);
        listener = Socket(::socket(AF_INET, SOCK_STREAM, 0));
        const int fd = listener.get();
        ensure(fd >= 0, "cannot create listening socket");
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ensure(flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0,
               "cannot configure listening socket");
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        ensure(isIPv4Address(config_.bindAddress),
               "invalid bind address: " + config_.bindAddress);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        ensure(::inet_pton(AF_INET, config_.bindAddress.c_str(),
                           &address.sin_addr) == 1,
               "invalid bind address: " + config_.bindAddress);
        address.sin_port = htons(config_.port);
        ensure(::bind(fd, reinterpret_cast<sockaddr*>(&address),
                      sizeof(address)) == 0,
               "cannot bind backup server port");
        ensure(::listen(fd, static_cast<int>(config_.maxConnections)) == 0,
               "cannot listen on backup server port");
        std::cout << "backup-server listening on " << config_.bindAddress << ":"
                  << config_.port << "\n";
        while (!stopRequested_)
        {
            reap(false);
            pollfd state{fd, POLLIN, 0};
            const int ready = ::poll(&state, 1, 100);
            if (ready < 0 && errno == EINTR)
            {
                continue;
            }
            ensure(ready >= 0 && !(state.revents & (POLLERR | POLLNVAL)),
                   "listener poll failed");
            if (ready == 0 || stopRequested_)
            {
                continue;
            }
            Socket client(::accept(fd, nullptr, nullptr));
            if (!client.valid())
            {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
                    errno == ECONNABORTED)
                {
                    continue;
                }
                throw NetError("cannot accept client: " +
                               std::string(std::strerror(errno)));
            }
            if (sessions.size() >= config_.maxConnections)
            {
                if (tls)
                {
                    continue; // No plaintext responses on a TLS listener.
                }
                try
                {
                    setTimeouts(client.get(), 1);
                    sendError(client.get(), 0,
                              "server connection limit reached",
                              &stopRequested_);
                }
                catch (...)
                {
                }
                continue;
            }
            auto finished = std::make_shared<std::atomic_bool>(false);
            sessions.push_back(
                {std::thread(
                     [this, clientFd = client.get(), finished, tls]()
                     {
                         handleSession(clientFd, config_, &stopRequested_, tls);
                         finished->store(true);
                     }),
                 std::move(finished)});
            client.release(); // The worker now owns the accepted socket.
        }
        success = true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Server error: " << e.what() << "\n";
    }
    stopRequested_ = true;
    listener.close();
    reap(true);
    return success;
}

void BackupServer::stop()
{
    stopRequested_ = true;
}
} // namespace backup::network
