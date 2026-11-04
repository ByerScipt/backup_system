#include "core/internal.hpp"
#include "helpers.hpp"

void testArchiveAuthentication()
{
    // RFC 4231 cases cover both ordinary and over-block-size keys.
    const auto vector = [](const std::vector<uint8_t>& key,
                           const std::string& data, const char* expected)
    {
        check(hexDigest(detail::hmacSha256(key, {data.begin(), data.end()})) ==
                  expected,
              "HMAC-SHA-256 RFC 4231 vector failed");
    };
    vector(std::vector<uint8_t>(20, 0x0b), "Hi There",
           "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    vector({'J', 'e', 'f', 'e'}, "what do ya want for nothing?",
           "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    vector(std::vector<uint8_t>(131, 0xaa),
           "Test Using Larger Than Block-Size Key - Hash Key First",
           "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    TempDirectory temp;
    const auto source = temp.path / "source";
    fs::create_directory(source);
    writeBytes(source / "secret", {'s', 'e', 'c', 'r', 'e', 't'});
    for (const auto cipher :
         {EncryptionAlgorithm::ChaCha20, EncryptionAlgorithm::Aes256})
    {
        const auto archive = temp.path / (toString(cipher) + ".bak");
        BackupOptions backup;
        backup.encryption = cipher;
        backup.password = "authentication-test";
        check(BackupEngine::create(source.string(), archive.string(), backup)
                  .success,
              "cannot create authentication fixture");
        bool decoded = false;
        RestoreOptions restore;
        restore.password = "wrong-password";
        restore.progress = [&](const ProgressEvent& event)
        { decoded |= event.stage == "decrypt"; };
        const auto output = temp.path / (toString(cipher) + "-output");
        auto result =
            BackupEngine::restore(archive.string(), output.string(), restore);
        check(!result.success && !decoded && !fs::exists(output),
              "incorrect password produced decrypted output before rejection");
        check(!BackupEngine::verify(archive.string(), restore).success &&
                  !decoded,
              "standalone verification decoded with an incorrect password");
        check(BackupEngine::inspect(archive.string()).version == 4,
              "encrypted archives must use the authenticated format");
        restore.password = backup.password;
        check(BackupEngine::restore(archive.string(), output.string(), restore)
                  .success,
              "authenticated archive did not restore");
        const auto original = readBytes(archive);
        for (const auto offset :
             {size_t(32), size_t(48), size_t(120), original.size() - 1})
        {
            auto changed = original;
            changed[offset] ^= 1;
            // Replacing the ordinary checksum must not bypass authentication.
            if (offset == 120)
            {
                const auto size = readLe64(changed, 24);
                const auto digest = sha256(std::vector<uint8_t>(
                    changed.begin() + 112, changed.begin() + 112 + size));
                std::copy(digest.begin(), digest.end(), changed.begin() + 80);
            }
            const auto tampered = temp.path / "tampered.bak";
            writeBytes(tampered, changed);
            decoded = false;
            result = BackupEngine::restore(
                tampered.string(), (temp.path / "rejected").string(), restore);
            check(!result.success && !decoded &&
                      !fs::exists(temp.path / "rejected"),
                  "tampered archive was decoded before authentication");
            check(!BackupEngine::verify(tampered.string(), restore).success,
                  "tampered archive passed standalone verification");
        }
        // Old encrypted archives remain available by explicit opt-in only.
        // A stripped v4 tag must never silently downgrade authentication.
        auto legacy = original;
        legacy.resize(legacy.size() - 32);
        legacy[4] = 2;
        const auto old = temp.path / "legacy.bak";
        writeBytes(old, legacy);
        const auto legacyOutput = temp.path / (toString(cipher) + "-legacy");
        check(
            !BackupEngine::restore(old.string(), legacyOutput.string(), restore)
                 .success,
            "stripping authentication silently enabled legacy decoding");
        restore.allowLegacyEncryption = true;
        check(
            BackupEngine::restore(old.string(), legacyOutput.string(), restore)
                .success,
            "explicit legacy encrypted restore failed");
        check(BackupEngine::verify(old.string(), restore).success,
              "explicit legacy encrypted verification failed");
        std::atomic_bool cancelled{true};
        restore.cancel = &cancelled;
        check(!BackupEngine::verify(archive.string(), restore).success,
              "standalone verification ignored cancellation");
    }
}
