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

} // namespace nlx::file
