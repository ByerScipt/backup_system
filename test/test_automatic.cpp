#include "helpers.hpp"
#include <future>

void testAutomatic()
{
    using namespace std::chrono_literals;
    TempDirectory temp;
    const auto source = temp.path / "source";
    const auto output = temp.path / "snapshots";
    fs::create_directories(source);
    fs::create_directories(output);
    writeBytes(source / "data", {'o', 'n', 'e'});
    writeBytes(output / "manual.bak", {'k', 'e', 'e', 'p'});
    BackupOptions options;
    options.compression = CompressionAlgorithm::Lz77;
    options.encryption = EncryptionAlgorithm::Aes256;
    options.password = "automatic-test";
    AutomaticOptions automatic;
    automatic.interval = 60ms;
    automatic.maxBackups = 3;
    automatic.keep = 2;
    std::vector<fs::path> archives;
    options.progress = [&](const ProgressEvent& event)
    {
        if (event.stage == "automatic-backup")
        {
            archives.push_back(event.detail);
        }
    };
    auto result = BackupEngine::automatic(source.string(), output.string(),
                                          options, automatic);
    check(result.success && archives.size() == 3,
          "periodic backup failed: " + result.message);
    check(!fs::exists(archives.front()) && fs::exists(archives.back()) &&
              readBytes(output / "manual.bak") ==
                  std::vector<uint8_t>({'k', 'e', 'e', 'p'}),
          "retention removed unrelated data or retained expired snapshots");
    RestoreOptions restore;
    restore.password = options.password;
    check(BackupEngine::restore(archives.back().string(),
                                (temp.path / "restored").string(), restore)
                  .success &&
              readBytes(temp.path / "restored/source/data") ==
                  readBytes(source / "data"),
          "automatic archive did not preserve encryption or content");
    const auto previous = archives.back();
    automatic.maxBackups = 1;
    automatic.keep = 1;
    check(BackupEngine::automatic(source.string(), output.string(), options,
                                  automatic)
                  .success &&
              !fs::exists(previous),
          "retention did not work across automatic backup restarts");

    check(!BackupEngine::automatic(source.string(),
                                   (source / "recursive-output").string(),
                                   options, automatic)
                  .success &&
              !fs::exists(source / "recursive-output"),
          "automatic backup created its output inside the source");
    automatic.keep = 0;
    check(!BackupEngine::automatic(source.string(), output.string(), options,
                                   automatic)
               .success,
          "automatic backup accepted invalid retention");
    automatic.keep = 1;
    std::atomic_bool cancel{true};
    options.cancel = &cancel;
    check(!BackupEngine::automatic(source.string(), output.string(), options,
                                   automatic)
               .success,
          "automatic backup ignored pre-cancellation");
    cancel = false;
    automatic.maxBackups = 0;
    automatic.interval = 1h;
    options.progress = [&](const ProgressEvent& event)
    {
        if (event.stage == "automatic-backup")
        {
            cancel = true;
        }
    };
    const auto start = std::chrono::steady_clock::now();
    result = BackupEngine::automatic(source.string(), output.string(), options,
                                     automatic);
    check(!result.success &&
              result.message.find("cancelled") != std::string::npos &&
              std::chrono::steady_clock::now() - start < 2s,
          "automatic backup did not promptly cancel its timer");

#ifdef __linux__
    cancel = false;
    automatic.realtime = true;
    automatic.interval = 60ms;
    automatic.keep = 3;
    automatic.maxBackups = 3;
    archives.clear();
    options.progress = [&](const ProgressEvent& event)
    {
        if (event.stage != "automatic-backup")
        {
            return;
        }
        archives.push_back(event.detail);
        if (archives.size() == 1)
        {
            writeBytes(source / "data", {'t', 'w', 'o'});
        }
        else if (archives.size() == 2)
        {
            fs::remove(source / "data");
            fs::create_directory(source / "nested");
            writeBytes(source / "nested/new", {'n', 'e', 'w'});
        }
    };
    auto realtime = std::async(std::launch::async,
                               [&]()
                               {
                                   return BackupEngine::automatic(
                                       source.string(), output.string(),
                                       options, automatic);
                               });
    const bool ready = realtime.wait_for(3s) == std::future_status::ready;
    if (!ready)
    {
        cancel = true;
    }
    result = realtime.get();
    check(ready && result.success && archives.size() == 3,
          "real-time backup missed a filesystem change: " + result.message);
    check(BackupEngine::restore(archives.back().string(),
                                (temp.path / "realtime").string(), restore)
                  .success &&
              !fs::exists(temp.path / "realtime/source/data") &&
              readBytes(temp.path / "realtime/source/nested/new") ==
                  std::vector<uint8_t>({'n', 'e', 'w'}),
          "real-time backup lost additions, deletions or nested directories");
#endif
}
