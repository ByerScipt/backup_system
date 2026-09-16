#include "helpers.hpp"

#include <cerrno>

namespace
{
int failSyncCall = 0;

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
}
