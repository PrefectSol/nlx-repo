#include "core/file.h"

#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <linux/limits.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>

namespace nlx::file
{

namespace 
{

using stat_t = struct stat;

bool make_dir(const char *path, mode_t mode) noexcept
{
    int result = ::mkdir(path, mode);
    if (result == 0)
    {
        return true;
    }

    if (errno != EEXIST)
    {
        return false;
    }

    stat_t st;
    result = ::stat(path, &st);
    if (result != 0)
    {
        return false;
    }

    return S_ISDIR(st.st_mode);
}

ssize_t close_with(int fd, int error) noexcept
{
    ::close(fd);
    errno = error;

    return -1;
}

} // namespace

bool mkdir_p(const char *path) noexcept
{
    if (path == nullptr || *path == '\0') [[unlikely]]
    {
        return false;
    }

    const size_t size = ::strnlen(path, PATH_MAX);
    if (size >= PATH_MAX) [[unlikely]]
    {
        return false;
    }

    char buffer[PATH_MAX];
    std::memcpy(buffer, path, size + 1);

    size_t lenght = size;
    while (lenght > 1 && buffer[lenght - 1] == '/')
    {
        buffer[--lenght] = '\0';
    }

    constexpr mode_t mode = 0777;
    for (size_t i = 1; i < lenght; ++i)
    {
        if (buffer[i] != '/' || buffer[i - 1] == '/')
        {
            continue;
        }

        buffer[i] = '\0';
        if (!make_dir(buffer, mode))
        {
            return false;
        }

        buffer[i] = '/';
    }

    return make_dir(buffer, mode);
}

bool writable(const char *path) noexcept
{
    const int fd = ::open(path, O_TMPFILE | O_WRONLY | O_CLOEXEC, 0644);
    if (fd >= 0)
    {
        ::close(fd);
        return true;
    }
    
    const int error = errno;
    if (error != EOPNOTSUPP && error != EISDIR)
    {
        return false;
    }

    const int result = ::access(path, W_OK | X_OK);
    
    return result == 0;
}

ssize_t read_file(const char *path, char *buffer, size_t capacity) noexcept
{
    if (path == nullptr || *path == '\0' || buffer == nullptr || capacity == 0) [[unlikely]]
    {
        errno = EINVAL;
        return -1;
    }

    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) [[unlikely]]
    {
        return -1;
    }

    size_t total = 0;
    while (total < capacity)
    {
        const ssize_t bytes = ::read(fd, buffer + total, capacity - total);
        if (bytes < 0) [[unlikely]]
        {
            if (errno == EINTR)
            {
                continue;
            }

            return close_with(fd, errno);
        }

        if (bytes == 0)
        {
            break;
        }

        total += static_cast<size_t>(bytes);
    }

    ::close(fd);

    return static_cast<ssize_t>(total);
}

namespace detail
{

ssize_t get_utf8bom_size(const char *data, size_t size) noexcept
{
    if (data == nullptr || size == 0) [[unlikely]]
    {
        return 0;
    }

    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(data);

    if (size >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
    {
        return 3;
    }

    if (size >= 2 && ((bytes[0] == 0xFE && bytes[1] == 0xFF) || (bytes[0] == 0xFF && bytes[1] == 0xFE)))
    {
        return -1;
    }

    if (size >= 4 && bytes[0] == 0x00 && bytes[1] == 0x00 && bytes[2] == 0xFE && bytes[3] == 0xFF)
    {
        return -1;
    }

    return 0;
}

} // namespace nlx::file::detail
} // namespace nlx::file
