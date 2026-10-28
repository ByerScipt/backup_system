#include "internal.hpp"
#include <climits>
#include <csignal>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <pthread.h>

namespace backup::network::detail
{
namespace
{
thread_local std::map<int, SSL*> channels;

void tlsEnsure(bool condition, const char* message)
{
    if (!condition)
    {
        char reason[256]{};
        const auto error = ERR_get_error();
        ERR_error_string_n(error, reason, sizeof(reason));
        throw NetError(std::string(message) +
                       (error ? ": " + std::string(reason) : ""));
    }
}

// OpenSSL writes can raise SIGPIPE. Block it only on this connection's thread,
// consume newly generated signals, then restore the caller's signal mask.
class SigpipeBlock
{
public:
    SigpipeBlock()
    {
        sigemptyset(&set_);
        sigaddset(&set_, SIGPIPE);
        ensure(pthread_sigmask(SIG_BLOCK, &set_, &previous_) == 0,
               "cannot protect TLS writes from SIGPIPE");
        sigset_t pending;
        sigpending(&pending);
        pending_ = sigismember(&pending, SIGPIPE) == 1;
    }
    ~SigpipeBlock()
    {
        if (!pending_ && sigismember(&previous_, SIGPIPE) != 1)
        {
            timespec now{};
            while (sigtimedwait(&set_, nullptr, &now) >= 0)
            {
            }
        }
        pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }

private:
    sigset_t set_{}, previous_{};
    bool pending_ = false;
};

TlsContext context(bool server)
{
    TlsContext result(
        SSL_CTX_new(server ? TLS_server_method() : TLS_client_method()),
        SSL_CTX_free);
    tlsEnsure(result != nullptr && SSL_CTX_set_min_proto_version(
                                       result.get(), TLS1_2_VERSION) == 1,
              "cannot initialize TLS context");
    SSL_CTX_set_options(result.get(), SSL_OP_NO_COMPRESSION);
    return result;
}

void retry(SSL* ssl, int result, int fd, IoClock::time_point deadline,
           std::atomic_bool* cancel)
{
    const int error = SSL_get_error(ssl, result);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
    {
        waitReady(fd, error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline,
                  cancel);
        return;
    }
    if (error == SSL_ERROR_SYSCALL && errno == EINTR)
    {
        return;
    }
    // Fatal TLS errors prohibit further SSL I/O, including close_notify.
    SSL_set_shutdown(ssl, SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);
    const auto verified = SSL_get_verify_result(ssl);
    if (verified != X509_V_OK)
    {
        throw NetError(std::string("TLS certificate verification failed: ") +
                       X509_verify_cert_error_string(verified));
    }
    tlsEnsure(false, "TLS connection failed");
}
} // namespace

TlsContext clientTlsContext(const TlsOptions& options)
{
    if (!options.enabled)
    {
        return {};
    }
    auto result = context(false);
    SSL_CTX_set_verify(result.get(), SSL_VERIFY_PEER, nullptr);
    ensure(options.caFile.find('\0') == std::string::npos,
           "invalid TLS CA file path");
    tlsEnsure(options.caFile.empty()
                  ? SSL_CTX_set_default_verify_paths(result.get()) == 1
                  : SSL_CTX_load_verify_locations(
                        result.get(), options.caFile.c_str(), nullptr) == 1,
              "cannot load TLS trust store");
    return result;
}

TlsContext serverTlsContext(const ServerConfig& config)
{
    if (config.tlsCertificate.empty() && config.tlsPrivateKey.empty())
    {
        return {};
    }
    ensure(!config.tlsCertificate.empty() && !config.tlsPrivateKey.empty() &&
               config.tlsCertificate.find('\0') == std::string::npos &&
               config.tlsPrivateKey.find('\0') == std::string::npos,
           "TLS requires both certificate and private key paths");
    auto result = context(true);
    SSL_CTX_set_default_passwd_cb(result.get(),
                                  [](char*, int, int, void*) { return 0; });
    tlsEnsure(SSL_CTX_use_certificate_chain_file(
                  result.get(), config.tlsCertificate.c_str()) == 1 &&
                  SSL_CTX_use_PrivateKey_file(result.get(),
                                              config.tlsPrivateKey.c_str(),
                                              SSL_FILETYPE_PEM) == 1 &&
                  SSL_CTX_check_private_key(result.get()) == 1,
              "cannot load TLS certificate and private key");
    return result;
}

struct TlsSession::State
{
    SigpipeBlock signals;
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl{nullptr, SSL_free};
    int fd = -1;
    ~State()
    {
        channels.erase(fd);
        if (ssl && SSL_is_init_finished(ssl.get()) &&
            !(SSL_get_shutdown(ssl.get()) & SSL_SENT_SHUTDOWN))
        {
            SSL_shutdown(ssl.get()); // Nonblocking: never wait during cleanup.
        }
    }
};

TlsSession::TlsSession(int fd, const TlsContext& context, bool server,
                       const std::string& host, std::atomic_bool* cancel)
{
    if (!context)
    {
        return;
    }
    ensure(!tlsActive(fd), "socket already has a TLS session");
    state_ = std::make_unique<State>();
    state_->fd = fd;
    state_->ssl.reset(SSL_new(context.get()));
    auto* ssl = state_->ssl.get();
    const int flags = ::fcntl(fd, F_GETFL, 0);
    tlsEnsure(ssl && SSL_set_fd(ssl, fd) == 1 && flags >= 0 &&
                  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0,
              "cannot prepare TLS connection");
    if (!server)
    {
        unsigned char address[16];
        const bool ip = inet_pton(AF_INET, host.c_str(), address) == 1 ||
                        inet_pton(AF_INET6, host.c_str(), address) == 1;
        SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        tlsEnsure(ip ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl),
                                                     host.c_str()) == 1
                     : SSL_set1_host(ssl, host.c_str()) == 1 &&
                           SSL_set_tlsext_host_name(ssl, host.c_str()) == 1,
                  "cannot set TLS server identity");
    }
    const auto deadline = ioDeadline(fd, SO_RCVTIMEO);
    for (;;)
    {
        checkCancelled(cancel);
        ERR_clear_error();
        const int result = server ? SSL_accept(ssl) : SSL_connect(ssl);
        if (result == 1)
        {
            break;
        }
        retry(ssl, result, fd, deadline, cancel);
    }
    channels.emplace(fd, ssl);
}

TlsSession::~TlsSession() = default;

bool tlsActive(int fd)
{
    return channels.count(fd) != 0;
}

ssize_t tlsTransfer(int fd, uint8_t* data, size_t size, bool writing,
                    IoClock::time_point deadline, std::atomic_bool* cancel)
{
    auto* ssl = channels.at(fd);
    ensure(SSL_get_shutdown(ssl) != (SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN),
           "TLS connection is closed");
    for (;;)
    {
        checkCancelled(cancel);
        ERR_clear_error();
        const int count = static_cast<int>(std::min<size_t>(size, INT_MAX));
        const int result =
            writing ? SSL_write(ssl, data, count) : SSL_read(ssl, data, count);
        if (result > 0)
        {
            return result;
        }
        if (!writing && SSL_get_error(ssl, result) == SSL_ERROR_ZERO_RETURN)
        {
            return 0;
        }
        retry(ssl, result, fd, deadline, cancel);
    }
}
} // namespace backup::network::detail
