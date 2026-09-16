#pragma once

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
        if (fd_ >= 0)
        {
            ::close(fd_);
        }
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other)
        {
            if (fd_ >= 0)
            {
                ::close(fd_);
            }
            fd_ = other.release();
        }
        return *this;
    }
    int get() const
    {
        return fd_;
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

} // namespace backup::detail
