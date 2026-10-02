#pragma once

#include <filesystem>
#include <unistd.h>

namespace backup::detail
{

// Owns exactly one POSIX descriptor, including during exception unwinding.
// release() transfers responsibility to a caller that checks close() itself.
class UniqueFd
{
public:
    explicit UniqueFd(int fd = -1) : fd_(fd) {}
    ~UniqueFd()
    {
        close();
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other)
        {
            close();
            fd_ = other.release();
        }
        return *this;
    }
    int get() const
    {
        return fd_;
    }
    bool valid() const
    {
        return fd_ >= 0;
    }
    void close()
    {
        if (valid())
        {
            ::close(fd_);
            fd_ = -1;
        }
    }
    int release()
    {
        const int value = fd_;
        fd_ = -1;
        return value;
    }

private:
    int fd_;
};

class TempFile
{
public:
    explicit TempFile(const std::filesystem::path& directory =
                          std::filesystem::temp_directory_path());
    ~TempFile();
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void commitFileNoReplace(const std::filesystem::path& temporary,
                         const std::filesystem::path& output);

} // namespace backup::detail
