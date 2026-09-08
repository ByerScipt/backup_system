#include "internal.hpp"
#include <charconv>
#include <limits>

namespace backup::network {
using namespace detail;

ServerConfig ServerConfig::load(const std::string& path) {
    ServerConfig config;
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot open server config: " + path);
    std::string line;
    auto trim = [](std::string value) {
        auto first = value.find_first_not_of(" \t\r\n");
        auto last = value.find_last_not_of(" \t\r\n");
        return first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
    };
    auto parseNumber = [](const std::string& value, uint32_t maximum) {
        uint32_t number = 0;
        auto result = std::from_chars(value.data(), value.data() + value.size(), number);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || number == 0 ||
            number > maximum)
            throw std::runtime_error("invalid numeric server setting: " + value);
        return number;
    };
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;
        auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        std::string key = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
        if (key == "port")
            config.port = static_cast<uint16_t>(parseNumber(value, 65535));
        else if (key == "storage_path")
            config.storagePath = value;
        else if (key == "max_connections")
            config.maxConnections = parseNumber(value, 1024);
        else if (key == "timeout_seconds")
            config.timeoutSeconds = parseNumber(value, std::numeric_limits<uint32_t>::max());
    }
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
                if (it->worker.joinable())
                    it->worker.join();
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
    };
    bool success = false;
    try {
        fs::create_directories(config_.storagePath);
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        ensure(fd >= 0, "cannot create listening socket");
        listenFd_.store(fd);
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(config_.port);
        ensure(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
               "cannot bind backup server port");
        ensure(::listen(fd, static_cast<int>(config_.maxConnections)) == 0,
               "cannot listen on backup server port");
        running_ = true;
        std::cout << "backup-server listening on 0.0.0.0:" << config_.port << "\n";
        while (running_) {
            int client = ::accept(fd, nullptr, nullptr);
            if (client < 0) {
                if (errno == EINTR)
                    continue;
                if (!running_)
                    break;
                continue;
            }
            reap(false);
            if (sessions.size() >= config_.maxConnections) {
                try {
                    sendError(client, 0, "server connection limit reached");
                } catch (...) {
                }
                ::close(client);
                continue;
            }
            auto finished = std::make_shared<std::atomic_bool>(false);
            sessions.push_back({std::thread([client, config = config_, finished]() {
                                    handleSession(client, config);
                                    finished->store(true);
                                }),
                                std::move(finished)});
        }
        success = true;
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << "\n";
    }
    running_ = false;
    int listening = listenFd_.exchange(-1);
    if (listening >= 0)
        ::close(listening);
    reap(true);
    return success;
}

void BackupServer::stop() {
    running_ = false;
    int listening = listenFd_.exchange(-1);
    if (listening >= 0) {
        ::shutdown(listening, SHUT_RDWR);
        ::close(listening);
    }
}
} // namespace backup::network
