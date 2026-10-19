#include "core/file_descriptor.h"
#include "core/fatal.h"
#include "core/system_error.h"
#include "core/time.h"

#include <csignal>
#include <utility>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/signalfd.h>

namespace nlx
{


FileDescriptor::FileDescriptor() noexcept :
    m_fd(-1)
{

}

FileDescriptor::FileDescriptor(int fd) noexcept :
    m_fd(fd)
{

}

FileDescriptor::~FileDescriptor() noexcept
{
    reset();
}

FileDescriptor::FileDescriptor(FileDescriptor &&other) noexcept :
    m_fd(std::exchange(other.m_fd, -1))
{

}

FileDescriptor &FileDescriptor::operator=(FileDescriptor &&other) noexcept
{
    if (this != &other)
    {
        reset();
        m_fd = std::exchange(other.m_fd, -1);
    }

    return *this;
}

bool FileDescriptor::operator==(const FileDescriptor &other) const noexcept
{
    return m_fd == other.m_fd;
}

bool FileDescriptor::operator==(int other) const noexcept
{
    return m_fd == other;
}

void FileDescriptor::reset() noexcept
{
    const int fd = std::exchange(m_fd, -1);
    if (fd < 0)
    {
        return;
    }
    
    const int result = ::close(fd);
    if (result != 0 && errno != EINTR) [[unlikely]]
    {
        NLX_FATAL("nlx::FileDescriptor::reset() close failed");
    }
}

int FileDescriptor::get() const noexcept
{
    return m_fd;
}

bool FileDescriptor::valid() const noexcept
{
    return m_fd >= 0;
}

FileDescriptor FileDescriptor::make_epoll()
{
    const int fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0) [[unlikely]]
    {
        throw SystemError("epoll_create1 failed");
    }

    return FileDescriptor(fd);
}

FileDescriptor FileDescriptor::make_eventfd(bool nonblocking)
{
    const int flags = EFD_CLOEXEC | (nonblocking ? EFD_NONBLOCK : 0);
    const int fd = ::eventfd(0, flags);
    if (fd < 0) [[unlikely]]
    {
        throw SystemError("eventfd failed");
    }

    return FileDescriptor(fd);
}

FileDescriptor FileDescriptor::make_timerfd(milliseconds interval, bool nonblocking)
{
    const int flags = TFD_CLOEXEC | (nonblocking ? TFD_NONBLOCK : 0);
    const int fd = ::timerfd_create(CLOCK_MONOTONIC, flags);
    if (fd < 0) [[unlikely]]
    {
        throw SystemError("timerfd_create failed");
    }
    
    FileDescriptor descriptor(fd);
    
    const itimerspec spec = detail::to_itimerspec(interval);
    const int result = ::timerfd_settime(descriptor.get(), 0, &spec, nullptr);
    if (result != 0) [[unlikely]]
    {
        throw SystemError("timerfd_settime failed");
    }

    return descriptor;
}

FileDescriptor FileDescriptor::make_signalfd(std::initializer_list<int> signals, bool nonblocking)
{
    sigset_t mask{};
    int result = ::sigemptyset(&mask);

    for (int signo : signals)
    {
        result = ::sigaddset(&mask, signo);
        if (result != 0) [[unlikely]]
        {
            throw SystemError("sigaddset failed");
        }
    }
    
    result = ::sigprocmask(SIG_BLOCK, &mask, nullptr);
    if (result != 0) [[unlikely]]
    {
        throw SystemError("sigprocmask failed");
    }

    const int flags = SFD_CLOEXEC | (nonblocking ? SFD_NONBLOCK : 0);
    const int fd = ::signalfd(-1, &mask, flags);
    if (fd < 0) [[unlikely]]
    {
        throw SystemError("signalfd failed");
    }

    return FileDescriptor(fd);
}

FileDescriptor FileDescriptor::open_append(const char *path) noexcept
{
    int fd;
    do
    {
        fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }
    while (fd < 0 && errno == EINTR);

    return FileDescriptor(fd);
}

namespace detail 
{

void epoll_add(const FileDescriptor &epoll_fd, const FileDescriptor &fd, uint32_t events)
{
    epoll_event event{};
    event.events = events;
    event.data.fd = fd.get();

    const int result = ::epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, fd.get(), &event);
    if (result != 0) [[unlikely]]
    {
        throw SystemError("epoll_ctl failed");
    }
}

size_t epoll_wait(const FileDescriptor &epoll_fd, std::span<epoll_event> events, int timeout) noexcept
{
    const int result = ::epoll_wait(epoll_fd.get(), events.data(), static_cast<int>(events.size()), timeout);
    if (result < 0) [[unlikely]]
    {
        if (errno == EINTR)
        {
            return 0;
        }

        NLX_FATAL("nlx::detail::epoll_wait() failed");
    }

    return static_cast<size_t>(result);
}

void notify(const FileDescriptor &fd) noexcept
{
    const uint64_t value = 1;
    ssize_t result;

    do
    {
        result = ::write(fd.get(), &value, sizeof(value));
    }
    while (result < 0 && errno == EINTR);

    if (result != sizeof(value)) [[unlikely]]
    {
        NLX_FATAL("nlx::detail::notify() write failed");
    }
}

int receive(const FileDescriptor &fd) noexcept
{
    signalfd_siginfo info{};
    ssize_t result;

    do
    {
        result = ::read(fd.get(), &info, sizeof(info));
    }
    while (result < 0 && errno == EINTR);

    if (result == sizeof(info))
    {
        return static_cast<int>(info.ssi_signo);
    }

    if (result < 0 && errno == EAGAIN)
    {
        return 0;
    }

    NLX_FATAL("nlx::detail::receive() read failed");
}

uint64_t drain(const FileDescriptor &fd) noexcept
{
    uint64_t value = 0;
    ssize_t result;

    do
    {
        result = ::read(fd.get(), &value, sizeof(value));
    }
    while (result < 0 && errno == EINTR);

    if (result == sizeof(value))
    {
        return value;
    }

    if (result < 0 && errno == EAGAIN)
    {
        return 0;
    }

    NLX_FATAL("nlx::detail::drain() read failed");
}

bool writeTo(const FileDescriptor &fd, const char *data, size_t size) noexcept
{
    size_t offset = 0;
    while (offset < size)
    {
        const ssize_t result = ::write(fd.get(), data + offset, size - offset);
        if (result <= 0) [[unlikely]]
        {
            if (result < 0 && errno == EINTR)
            {
                continue;
            }

            return false;
        }

        offset += static_cast<size_t>(result);
    }

    return true;
}

void wait(const FileDescriptor &fd) noexcept
{
    pollfd descriptor{};
    descriptor.fd = fd.get();
    descriptor.events = POLLIN;

    while (true)
    {
        const int result = ::poll(&descriptor, 1, -1);
        if (result < 0 && errno == EINTR)
        {
            continue;
        }

        if (result < 0) [[unlikely]]
        {
            NLX_FATAL("nlx::detail::wait() poll failed");
        }

        if (drain(fd) != 0)
        {
            return;
        }
    }
}

} // namespace nlx::detail
} // namespace nlx
