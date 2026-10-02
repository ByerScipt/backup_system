#include "core/internal.hpp"
#include "helpers.hpp"
#include "network/internal.hpp"
#include <cerrno>
#include <csignal>
#include <sys/wait.h>

namespace
{
std::atomic_int failSyncCall{0};

size_t descriptorCount()
{
    return static_cast<size_t>(std::distance(
        fs::directory_iterator("/proc/self/fd"), fs::directory_iterator{}));
}
} // namespace

extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd)
{
    if (failSyncCall > 0 && --failSyncCall == 0)
    {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

void testSyncFailure()
{
    TempDirectory temp;
    const auto source = temp.path / "source";
    fs::create_directory(source);
    writeBytes(source / "file", {'d', 'a', 't', 'a'});
    for (int call : {1, 2})
    {
        const auto before = descriptorCount();
        const auto archive =
            temp.path / ("sync-" + std::to_string(call) + ".bak");
        failSyncCall = call;
        const auto result =
            BackupEngine::create(source.string(), archive.string(), {});
        const bool injected = failSyncCall == 0;
        failSyncCall = 0;
        check(injected && !result.success, "fsync failure was not reported");
        check(result.message.find("synchronize") != std::string::npos,
              "fsync failure did not explain the error");
        check(descriptorCount() == before, "fsync failure leaked a descriptor");
        if (call == 1)
        {
            check(!fs::exists(archive), "unsynchronized file was published");
        }
        else
        {
            // Publication precedes parent synchronization. Report uncertainty,
            // but retain the valid archive rather than deleting user output.
            check(BackupEngine::inspect(archive.string()).encodedSize > 0,
                  "parent sync failure lost the already-published archive");
        }
    }
    namespace storage = network::detail;
    for (bool account : {true, false})
    {
        const auto directory = temp.path / (account ? "accounts" : "metadata");
        fs::create_directory(directory);
        const auto path = directory / (account ? "users.db" : "backup.meta");
        storage::UserRecord user;
        user.name = "alice";
        network::RemoteBackupEntry entry;
        entry.name = "original";
        entry.digest = hexDigest(sha256(std::string("data")));
        auto write = [&]()
        {
            if (account)
            {
                storage::saveUsers(directory, {user});
            }
            else
            {
                storage::writeMeta(path, entry);
            }
        };
        write();
        const auto original = readBytes(path);
        user.name = "bob";
        entry.name = "updated";
        failSyncCall = 1;
        bool failed = false;
        try
        {
            write();
        }
        catch (const std::exception&)
        {
            failed = true;
        }
        const bool injected = failSyncCall == 0;
        failSyncCall = 0;
        check(injected && failed, "storage did not synchronize before commit");
        check(readBytes(path) == original,
              "failed storage synchronization replaced existing data");
        check(std::distance(fs::directory_iterator(directory),
                            fs::directory_iterator{}) == 1,
              "failed storage write left temporary files");
    }
}

// White-box I/O seams: /dev/full reports ENOSPC without filling a real volume.
// Small writes exercise the final stream flush, not just writeExact().
void testUploadSyncFailure()
{
    TempDirectory temp;
    const auto archive = temp.path / "input.bak";
    writeBytes(archive, {'t', 'e', 's', 't'});
    network::ServerConfig config;
    config.port = reservePort();
    config.storagePath = (temp.path / "storage").string();
    check(config.port != 0, "cannot reserve sync regression port");
    network::BackupServer server(config);
    std::atomic_bool serverOkay{false};
    std::thread thread([&] { serverOkay = server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    try
    {
        network::BackupClient client("127.0.0.1", config.port, "sync_user",
                                     "password");
        std::string error;
        check(client.registerUser(error),
              "cannot register sync fixture: " + error);
        for (int call : {3, 4})
        {
            failSyncCall =
                call; // Uploaded file + directory, metadata file + directory.
            std::string id;
            const bool uploaded =
                client.upload(archive.string(), "sync", id, error);
            const bool injected = failSyncCall == 0;
            failSyncCall = 0;
            check(injected && !uploaded,
                  "metadata synchronization failure was not reported");
            check(client.list(error).empty() && error.empty(),
                  "failed upload remained listed");
            const auto dir =
                network::detail::userDirectory(config.storagePath, "sync_user");
            check(fs::is_empty(dir),
                  "failed upload left an archive or dangling metadata");
        }
    }
    catch (...)
    {
        failSyncCall = 0;
        server.stop();
        thread.join();
        throw;
    }
    server.stop();
    thread.join();
    check(serverOkay, "sync regression server failed");
}

void testPipelineIo()
{
    namespace pipeline = backup::detail;
    TempDirectory temp;
    const auto input = temp.path / "input";
    writeBytes(input, {'s', 'm', 'a', 'l', 'l'});
    const auto source = temp.path / "source";
    fs::create_directory(source);
    uint64_t size = 0;
    const auto entries = pipeline::scanDirectory(source, size, {});
    const auto rle = temp.path / "rle";
    const auto huffman = temp.path / "huffman";
    pipeline::rleCompress(input, rle, {});
    pipeline::huffmanCompress(input, huffman, {});
    const std::vector<std::pair<std::string, std::function<void()>>> operations{
        {"stream pack",
         [&] { pipeline::packStream(entries, "/dev/full", size, {}); }},
        {"index pack",
         [&] { pipeline::packIndex(entries, "/dev/full", size, {}); }},
        {"copy", [&]
         { pipeline::copyFileStage(input, "/dev/full", "copy", {}, nullptr); }},
        {"RLE encode", [&] { pipeline::rleCompress(input, "/dev/full", {}); }},
        {"RLE decode",
         [&] { pipeline::rleDecompress(rle, "/dev/full", 5, {}); }},
        {"Huffman encode",
         [&] { pipeline::huffmanCompress(input, "/dev/full", {}); }},
        {"Huffman decode",
         [&] { pipeline::huffmanDecompress(huffman, "/dev/full", 5, {}); }},
        {"ChaCha20",
         [&]
         {
             pipeline::cryptStage(input, "/dev/full",
                                  EncryptionAlgorithm::ChaCha20, "key", {}, {},
                                  nullptr, "encrypt");
         }},
        {"AES",
         [&]
         {
             pipeline::cryptStage(input, "/dev/full",
                                  EncryptionAlgorithm::Aes256, "key", {}, {},
                                  nullptr, "encrypt");
         }},
        {"no encryption", [&]
         {
             pipeline::cryptStage(input, "/dev/full", EncryptionAlgorithm::None,
                                  {}, {}, {}, nullptr, "copy");
         }}};
    for (const auto& operation : operations)
    {
        bool rejected = false;
        try
        {
            operation.second();
        }
        catch (const std::exception&)
        {
            rejected = true;
        }
        check(rejected,
              operation.first + " silently ignored a full output device");
    }
}

void testCancellation()
{
    TempDirectory temp;
    const auto source = temp.path / "source";
    fs::create_directory(source);
    writeBytes(source / "file", {'d', 'a', 't', 'a'});
    const auto archive = temp.path / "cancelled.bak";
    std::atomic_bool cancel{false};
    BackupOptions options;
    options.cancel = &cancel;
    options.progress = [&](const ProgressEvent& event)
    {
        if (event.stage == "finalize" && event.completed == event.total)
        {
            cancel = true;
        }
    };
    auto result =
        BackupEngine::create(source.string(), archive.string(), options);
    check(!result.success && !fs::exists(archive),
          "final-stage cancellation published a successful archive");
    check(BackupEngine::create(source.string(), archive.string(), {}).success,
          "cannot create cancellation fixture");
    cancel = false;
    RestoreOptions restore;
    restore.cancel = &cancel;
    restore.progress = [&](const ProgressEvent& event)
    {
        if (event.stage == "extract" && event.completed == event.total)
        {
            cancel = true;
        }
    };
    const auto destination = temp.path / "restored";
    result =
        BackupEngine::restore(archive.string(), destination.string(), restore);
    check(!result.success && !fs::exists(destination / "source/file"),
          "final-stage cancellation committed restored file");
    cancel = false;
    bool rejected = false;
    try
    {
        BackupEngine::preview(
            archive.string(), destination.string(), {},
            [&](const ProgressEvent&) { cancel = true; }, &cancel);
    }
    catch (const std::exception& error)
    {
        rejected =
            std::string(error.what()).find("cancelled") != std::string::npos;
    }
    check(rejected, "archive preview ignored cancellation");
}

void testSourceFifoRace()
{
    TempDirectory temp;
    const auto source = temp.path / "source";
    fs::create_directory(source);
    writeBytes(source / "file", {'x'});
    // Isolate the old blocking open so a regression fails instead of hanging
    // CI.
    const pid_t child = ::fork();
    check(child >= 0, "cannot fork source race fixture");
    if (child == 0)
    {
        BackupOptions options;
        options.progress = [&](const ProgressEvent& event)
        {
            if (event.stage == "scan" && event.detail == "source/file")
            {
                fs::remove(source / "file");
                if (::mkfifo((source / "file").c_str(), 0600) != 0)
                {
                    ::_exit(2);
                }
            }
        };
        const auto result = BackupEngine::create(
            source.string(), (temp.path / "race.bak").string(), options);
        ::_exit(!result.success &&
                        result.message.find("changed") != std::string::npos
                    ? 0
                    : 3);
    }
    int status = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (::waitpid(child, &status, WNOHANG) == 0)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            ::kill(child, SIGKILL);
            ::waitpid(child, &status, 0);
            throw std::runtime_error("source replaced by FIFO blocked backup");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "source FIFO replacement was not rejected");
    check(!fs::exists(temp.path / "race.bak"), "source race published archive");
}
