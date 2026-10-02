#include "helpers.hpp"
#include "network/internal.hpp"
#include <poll.h>

namespace
{
namespace net = backup::network::detail;

void acceptLogin(int fd)
{
    auto request = net::receiveFrame(fd);
    check(request && request->type == net::MessageType::LoginStart,
          "missing login request");
    net::sendFrame(fd, net::MessageType::LoginChallenge, request->requestId,
                   std::vector<uint8_t>(32));
    auto proof = net::receiveFrame(fd);
    check(proof && proof->requestId == request->requestId,
          "missing login proof");
    net::sendFrame(fd, net::MessageType::LoginResponse, request->requestId,
                   {0});
}

class ScriptedServer
{
public:
    using Handler = std::function<void(int, std::atomic_bool&)>;
    explicit ScriptedServer(Handler handler)
        : listener_(::socket(AF_INET, SOCK_STREAM, 0))
    {
        check(listener_.valid(), "cannot create scripted listener");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address),
                     sizeof(address)) == 0,
              "cannot bind scripted listener");
        socklen_t size = sizeof(address);
        check(::getsockname(listener_.get(),
                            reinterpret_cast<sockaddr*>(&address), &size) == 0,
              "cannot inspect scripted listener");
        port = ntohs(address.sin_port);
        check(::listen(listener_.get(), 1) == 0,
              "cannot listen for scripted client");
        worker_ = std::thread(
            [this, handler]()
            {
                try
                {
                    pollfd ready{listener_.get(), POLLIN, 0};
                    if (::poll(&ready, 1, 2000) <= 0)
                    {
                        return;
                    }
                    net::Socket peer(
                        ::accept(listener_.get(), nullptr, nullptr));
                    check(peer.valid(), "scripted accept failed");
                    net::setTimeouts(peer.get(), 2);
                    handler(peer.get(), finished_);
                }
                catch (...)
                {
                    failure_ = std::current_exception();
                }
            });
    }
    ~ScriptedServer()
    {
        finished_ = true;
        if (worker_.joinable())
        {
            worker_.join();
        }
    }
    void verify()
    {
        finished_ = true;
        worker_.join();
        if (failure_)
        {
            std::rethrow_exception(failure_);
        }
    }
    uint16_t port = 0;

private:
    net::Socket listener_;
    std::atomic_bool finished_{false};
    std::thread worker_;
    std::exception_ptr failure_;
};
} // namespace

void testNetworkFailures()
{
    // Login waits for a peer that accepts TCP but never sends its challenge.
    ScriptedServer stalled(
        [](int fd, std::atomic_bool& finished)
        {
            check(net::receiveFrame(fd).has_value(), "missing login request");
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!finished && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    backup::network::BackupClient client("127.0.0.1", stalled.port, "user",
                                         "password");
    std::atomic_bool cancelled{false};
    std::thread canceller(
        [&]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            cancelled = true;
        });
    std::string id, error;
    const auto started = std::chrono::steady_clock::now();
    bool uploaded = client.upload("unused", "test", id, error, {}, &cancelled);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    canceller.join();
    stalled.verify();
    check(!uploaded && error.find("cancelled") != std::string::npos &&
              elapsed < std::chrono::milliseconds(1500),
          "client could not cancel an unresponsive login: " + error);

    ScriptedServer mismatched(
        [](int fd, std::atomic_bool&)
        {
            check(net::receiveFrame(fd).has_value(),
                  "missing registration request");
            net::sendFrame(fd, net::MessageType::RegisterResponse, 999, {0});
        });
    backup::network::BackupClient registration("127.0.0.1", mismatched.port,
                                               "user", "password");
    bool registered = registration.registerUser(error);
    mismatched.verify();
    check(!registered && !error.empty(),
          "accepted registration with wrong request ID");

    TempDirectory temp;
    const auto archive = temp.path / "empty.bak";
    writeBytes(archive, {});
    for (bool wrongRequest : {true, false})
    {
        ScriptedServer uploadServer(
            [wrongRequest](int fd, std::atomic_bool&)
            {
                acceptLogin(fd);
                auto start = net::receiveFrame(fd);
                check(start && start->type == net::MessageType::UploadStart,
                      "missing upload start");
                std::vector<uint8_t> payload;
                net::putString(payload, std::string(32, 'a'));
                net::sendFrame(fd, net::MessageType::UploadReady,
                               start->requestId, payload);
                auto end = net::receiveFrame(fd);
                check(end && end->type == net::MessageType::UploadEnd,
                      "missing empty upload end");
                payload.clear();
                net::putString(payload,
                               std::string(32, wrongRequest ? 'a' : 'b'));
                net::sendFrame(fd, net::MessageType::UploadResult,
                               start->requestId + (wrongRequest ? 1 : 0),
                               payload);
            });
        backup::network::BackupClient uploader("127.0.0.1", uploadServer.port,
                                               "user", "password");
        id = "previous-upload-id";
        const bool ok = uploader.upload(archive.string(), "test", id, error);
        uploadServer.verify();
        check(!ok && !error.empty() && id.empty(),
              "accepted mismatched upload acknowledgement or exposed "
              "provisional ID");
    }

    ScriptedServer downloadServer(
        [](int fd, std::atomic_bool&)
        {
            acceptLogin(fd);
            auto request = net::receiveFrame(fd);
            check(request && request->type == net::MessageType::DownloadRequest,
                  "missing download request");
            std::vector<uint8_t> payload;
            net::putU64(payload, 0);
            const auto digest = sha256(std::string());
            payload.insert(payload.end(), digest.begin(), digest.end());
            net::sendFrame(fd, net::MessageType::DownloadStart,
                           request->requestId, payload);
            net::sendFrame(fd, net::MessageType::DownloadEnd,
                           request->requestId, {1});
        });
    backup::network::BackupClient downloader("127.0.0.1", downloadServer.port,
                                             "user", "password");
    const auto output = temp.path / "download.bak";
    bool downloaded =
        downloader.download(std::string(32, 'a'), output.string(), error);
    downloadServer.verify();
    check(!downloaded && !fs::exists(output),
          "malformed download end published output");

    int sockets[2];
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
          "cannot create timeout fixture");
    net::Socket reader(sockets[0]), writer(sockets[1]);
    timeval timeout{0, 100000};
    check(::setsockopt(reader.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) == 0,
          "cannot set timeout fixture");
    bool timedOut = false;
    try
    {
        net::receiveFrame(reader.get());
    }
    catch (const std::exception& e)
    {
        timedOut = std::string(e.what()).find("timed out") != std::string::npos;
    }
    check(timedOut, "socket wait did not report a bounded timeout");
}
