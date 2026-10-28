#include "helpers.hpp"
#include "network/internal.hpp"
#include <openssl/pem.h>
#include <openssl/x509v3.h>

namespace
{
void certificate(const fs::path& cert, const fs::path& key,
                 bool expired = false)
{
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    check(generator && EVP_PKEY_keygen_init(generator.get()) == 1 &&
              EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048) == 1,
          "cannot initialize TLS test key");
    EVP_PKEY* raw = nullptr;
    check(EVP_PKEY_keygen(generator.get(), &raw) == 1,
          "cannot create TLS test key");
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pair(raw,
                                                             EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> x509(X509_new(), X509_free);
    check(x509 && X509_set_version(x509.get(), 2) == 1 &&
              ASN1_INTEGER_set(X509_get_serialNumber(x509.get()), 1) == 1 &&
              X509_gmtime_adj(X509_getm_notBefore(x509.get()), -3600) &&
              X509_gmtime_adj(X509_getm_notAfter(x509.get()),
                              expired ? -60 : 3600) &&
              X509_set_pubkey(x509.get(), pair.get()) == 1,
          "cannot create TLS test certificate");
    auto* name = X509_get_subject_name(x509.get());
    check(X509_NAME_add_entry_by_txt(
              name, "CN", MBSTRING_ASC,
              reinterpret_cast<const unsigned char*>("localhost"), -1, -1,
              0) == 1 &&
              X509_set_issuer_name(x509.get(), name) == 1,
          "cannot set TLS certificate subject");
    for (const auto& extension : std::vector<std::pair<int, const char*>>{
             {NID_basic_constraints, "critical,CA:TRUE"},
             {NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1"}})
    {
        std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> value(
            X509V3_EXT_conf_nid(nullptr, nullptr, extension.first,
                                extension.second),
            X509_EXTENSION_free);
        check(value && X509_add_ext(x509.get(), value.get(), -1) == 1,
              "cannot set TLS certificate extensions");
    }
    check(X509_sign(x509.get(), pair.get(), EVP_sha256()) > 0,
          "cannot sign TLS certificate");
    std::unique_ptr<FILE, decltype(&fclose)> out(fopen(cert.c_str(), "w"),
                                                 fclose);
    check(out && PEM_write_X509(out.get(), x509.get()) == 1,
          "cannot write TLS certificate");
    out.reset(fopen(key.c_str(), "w"));
    check(out && PEM_write_PrivateKey(out.get(), pair.get(), nullptr, nullptr,
                                      0, nullptr, nullptr) == 1,
          "cannot write TLS private key");
    check(chmod(key.c_str(), 0600) == 0, "cannot secure TLS test key");
}
} // namespace

void testTls()
{
    namespace protocol = network::detail;
    TempDirectory temp;
    const auto cert = temp.path / "server.pem", key = temp.path / "server.key";
    certificate(cert, key);
    network::ServerConfig config;
    config.port = reservePort();
    config.storagePath = (temp.path / "storage").string();
    config.tlsCertificate = cert.string();
    config.tlsPrivateKey = key.string();
    config.timeoutSeconds = 2;
    check(config.port != 0, "cannot reserve TLS port");
    network::BackupServer server(config);
    std::atomic_bool success{false};
    std::thread service([&]() { success = server.run(); });
    try
    {
        network::TlsOptions trusted{true, cert.string()};
        network::BackupClient client("127.0.0.1", config.port, "tls",
                                     "password", trusted);
        std::string error;
        bool registered = false;
        for (int attempt = 0; attempt < 50 && !registered; ++attempt)
        {
            registered = client.registerUser(error);
            if (!registered)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        check(registered && client.login(error),
              "verified TLS login failed: " + error);
        network::BackupClient wrongPassword("localhost", config.port, "tls",
                                            "wrong", trusted);
        check(!wrongPassword.login(error) &&
                  error.find("invalid username or password") !=
                      std::string::npos,
              "TLS error responses were not encrypted or DNS verification "
              "failed");
        const auto archive = temp.path / "upload.bak";
        writeBytes(archive, std::vector<uint8_t>(1024 * 1024 + 17, 's'));
        std::string id, next;
        check(client.upload(archive.string(), "TLS", id, error),
              "TLS upload failed: " + error);
        size_t reused = 0;
        check(client.upload(
                  archive.string(), "TLS delta", next, error,
                  [&](const ProgressEvent& event)
                  { reused += event.stage == "network-reuse"; }, nullptr,
                  true) &&
                  reused > 0,
              "incremental upload over TLS failed: " + error);
        const auto downloaded = temp.path / "download.bak";
        check(client.download(next, downloaded.string(), error) &&
                  sha256File(downloaded.string()) ==
                      sha256File(archive.string()),
              "TLS download differs: " + error);
        network::BackupClient wrongHost("127.0.0.2", config.port, "tls",
                                        "password", trusted);
        check(!wrongHost.login(error) &&
                  error.find("certificate") != std::string::npos,
              "TLS accepted a server with the wrong identity");
        const auto unrelated = temp.path / "unrelated.pem";
        certificate(unrelated, temp.path / "unrelated.key");
        network::BackupClient untrusted("127.0.0.1", config.port, "tls",
                                        "password", {true, unrelated.string()});
        check(!untrusted.login(error) &&
                  error.find("certificate") != std::string::npos,
              "TLS accepted an untrusted server certificate");
        network::BackupClient plain("127.0.0.1", config.port, "tls",
                                    "password");
        check(!plain.login(error),
              "TLS listener accepted plaintext authentication");
        // A peer which never sends ClientHello must not prevent shutdown.
        auto idle = protocol::connectTo("127.0.0.1", config.port);
        const auto start = std::chrono::steady_clock::now();
        server.stop();
        service.join();
        check(success && std::chrono::steady_clock::now() - start <
                             std::chrono::seconds(2),
              "an idle TLS handshake prevented shutdown");
        config.tlsPrivateKey.clear();
        network::BackupServer incomplete(config);
        check(!incomplete.run(),
              "TLS server accepted an incomplete key configuration");
        certificate(temp.path / "expired.pem", key, true);
        config.tlsCertificate = (temp.path / "expired.pem").string();
        config.tlsPrivateKey = key.string();
        network::BackupServer expired(config);
        std::thread expiredService([&]() { expired.run(); });
        try
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            network::BackupClient old("127.0.0.1", config.port, "tls",
                                      "password",
                                      {true, config.tlsCertificate});
            check(!old.login(error) &&
                      error.find("expired") != std::string::npos,
                  "TLS accepted an expired certificate");
        }
        catch (...)
        {
            expired.stop();
            expiredService.join();
            throw;
        }
        expired.stop();
        expiredService.join();
    }
    catch (...)
    {
        server.stop();
        if (service.joinable())
        {
            service.join();
        }
        throw;
    }
}
