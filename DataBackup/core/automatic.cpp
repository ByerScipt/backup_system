#include "internal.hpp"
#include <cctype>
#include <chrono>
#include <iomanip>
#include <optional>
#include <thread>
#ifdef __linux__
#include <poll.h>
#include <sys/inotify.h>
#endif

namespace backup
{
namespace
{
using Clock = std::chrono::steady_clock;

std::string snapshotName(const std::string& prefix)
{
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    std::ostringstream name;
    name << prefix << std::setw(20) << std::setfill('0') << stamp << '-';
    const auto salt = detail::randomSalt();
    name << std::hex;
    for (size_t index = 0; index < 8; ++index)
    {
        name << std::setw(2) << static_cast<unsigned>(salt[index]);
    }
    return name.str() + ".bak";
}

void retainSnapshots(const fs::path& directory, const std::string& prefix,
                     const fs::path& current, size_t keep)
{
    std::vector<fs::path> previous;
    for (const auto& entry : fs::directory_iterator(directory))
    {
        const auto name = entry.path().filename().string();
        if (entry.path() == current ||
            entry.symlink_status().type() != fs::file_type::regular ||
            name.size() != prefix.size() + 41 || name.rfind(prefix, 0) != 0 ||
            name.substr(name.size() - 4) != ".bak" ||
            name[prefix.size() + 20] != '-')
        {
            continue;
        }
        const auto start = name.begin() + prefix.size();
        if (std::all_of(start, start + 20,
                        [](unsigned char c) { return c >= '0' && c <= '9'; }) &&
            std::all_of(start + 21, start + 37,
                        [](unsigned char c) { return std::isxdigit(c); }))
        {
            previous.push_back(entry.path());
        }
    }
    std::sort(previous.begin(), previous.end(), std::greater<fs::path>());
    for (size_t index = keep - 1; index < previous.size(); ++index)
    {
        detail::ensure(fs::remove(previous[index]),
                       "cannot remove expired automatic archive");
    }
}

#ifdef __linux__
class ChangeWatch
{
public:
    ChangeWatch() : fd_(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC))
    {
        detail::ensure(fd_.valid(), "cannot create filesystem watcher");
    }

    void refresh(const fs::path& source, std::atomic_bool* cancel)
    {
        for (const int watch : watches_)
        {
            ::inotify_rm_watch(fd_.get(), watch);
        }
        watches_.clear();
        const auto add = [&](const fs::path& path)
        {
            detail::checkCancelled(cancel);
            const int watch = ::inotify_add_watch(
                fd_.get(), path.c_str(),
                IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM |
                    IN_MOVED_TO | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF |
                    IN_ONLYDIR | IN_DONT_FOLLOW);
            detail::ensure(watch >= 0,
                           "cannot watch source directory: " + path.string());
            watches_.push_back(watch);
        };
        add(source);
        for (const auto& entry : fs::recursive_directory_iterator(source))
        {
            if (entry.symlink_status().type() == fs::file_type::directory)
            {
                add(entry.path());
            }
        }
    }

    bool changed()
    {
        alignas(inotify_event) char buffer[16384];
        bool changed = false;
        for (;;)
        {
            const auto count = ::read(fd_.get(), buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR)
            {
                continue;
            }
            if (count < 0 && errno == EAGAIN)
            {
                return changed;
            }
            detail::ensure(count > 0, "cannot read filesystem changes");
            for (size_t offset = 0; offset < static_cast<size_t>(count);)
            {
                const auto* event =
                    reinterpret_cast<const inotify_event*>(buffer + offset);
                changed |= (event->mask & ~IN_IGNORED) != 0;
                offset += sizeof(inotify_event) + event->len;
            }
        }
    }

private:
    detail::UniqueFd fd_;
    std::vector<int> watches_;
};
#endif
} // namespace

BackupResult BackupEngine::automatic(const std::string& sourceDirectory,
                                     const std::string& outputDirectory,
                                     const BackupOptions& options,
                                     const AutomaticOptions& automatic)
{
    BackupResult result;
    try
    {
        detail::ensure(automatic.keep > 0 && automatic.keep <= 10000 &&
                           automatic.interval >=
                               std::chrono::milliseconds(20) &&
                           automatic.interval <= std::chrono::hours(24 * 7) &&
                           !outputDirectory.empty(),
                       "invalid automatic backup settings");
        detail::checkCancelled(options.cancel);
        const fs::path source = fs::canonical(sourceDirectory);
        const fs::path output =
            fs::weakly_canonical(fs::absolute(outputDirectory));
        detail::ensure(!detail::isPathInside(output, source),
                       "automatic output cannot be inside the source");
        fs::create_directories(output);
        const std::string prefix =
            "auto-" + hexDigest(sha256(source.string())).substr(0, 16) + "-";
#ifdef __linux__
        std::optional<ChangeWatch> watch;
        if (automatic.realtime)
        {
            watch.emplace();
            watch->refresh(source, options.cancel);
        }
#else
        detail::ensure(!automatic.realtime,
                       "real-time backup requires Linux inotify");
#endif
        auto next = Clock::now();
        size_t completed = 0;
        bool pending = false;
        for (;;)
        {
            detail::checkCancelled(options.cancel);
#ifdef __linux__
            if (watch && watch->changed())
            {
                pending = true;
                next = Clock::now() + automatic.interval;
            }
#endif
            if ((completed == 0 || !automatic.realtime || pending) &&
                Clock::now() >= next)
            {
#ifdef __linux__
                if (watch && completed > 0)
                {
                    watch->refresh(source, options.cancel);
                }
#endif
                const auto archive = output / snapshotName(prefix);
                const auto created =
                    create(source.string(), archive.string(), options);
                detail::ensure(created.success, created.message);
                retainSnapshots(output, prefix, archive, automatic.keep);
                ++completed;
                result.entryCount += created.entryCount;
                result.inputBytes += created.inputBytes;
                result.outputBytes += created.outputBytes;
                detail::report(options.progress, "automatic-backup", completed,
                               automatic.maxBackups, archive.string());
                if (automatic.maxBackups && completed >= automatic.maxBackups)
                {
                    result.success = true;
                    result.message = "automatic backup completed";
                    return result;
                }
                pending = false;
                next = Clock::now() + automatic.interval;
            }
            std::this_thread::sleep_for(
                std::min(automatic.interval, std::chrono::milliseconds(100)));
        }
    }
    catch (const std::exception& error)
    {
        result.message = error.what();
    }
    return result;
}
} // namespace backup
