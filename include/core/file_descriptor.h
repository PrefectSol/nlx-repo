#ifndef NLX_CORE_FILE_DESCRIPTOR_H
#define NLX_CORE_FILE_DESCRIPTOR_H

#include "core/time.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <sys/epoll.h>

namespace nlx
{

class FileDescriptor
{
public:
    FileDescriptor() noexcept;
    ~FileDescriptor() noexcept;

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    FileDescriptor(FileDescriptor &&other) noexcept;
    FileDescriptor &operator=(FileDescriptor &&other) noexcept;

    void reset() noexcept;
    
    [[nodiscard]] int get() const noexcept;
    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] static FileDescriptor make_epoll();
    [[nodiscard]] static FileDescriptor make_eventfd(bool nonblocking = true);
    [[nodiscard]] static FileDescriptor make_timerfd(milliseconds interval, bool nonblocking = true);
    [[nodiscard]] static FileDescriptor make_signalfd(std::initializer_list<int> signals, bool nonblocking = true);
    [[nodiscard]] static FileDescriptor open_append(const char *path) noexcept;

private:
    explicit FileDescriptor(int fd) noexcept;

    int m_fd;
};

namespace detail
{

void epoll_add(const FileDescriptor &epoll_fd, const FileDescriptor &fd, uint32_t events = EPOLLIN);
[[nodiscard]] size_t epoll_wait(const FileDescriptor &epoll_fd, std::span<epoll_event> events, int timeout) noexcept;

void notify(const FileDescriptor &fd) noexcept;
[[nodiscard]] int receive(const FileDescriptor &fd) noexcept;
[[nodiscard]] uint64_t drain(const FileDescriptor &fd) noexcept;

[[nodiscard]] bool writeTo(const FileDescriptor &fd, const char *data, size_t size) noexcept;

} // namespace nlx::detail
} // namespace nlx
#endif // !NLX_CORE_FILE_DESCRIPTOR_H

