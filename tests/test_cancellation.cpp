#include "helpers.hpp"
#include <csignal>
#include <sys/wait.h>

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
