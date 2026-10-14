#ifndef NLX_CORE_FILE_H
#define NLX_CORE_FILE_H

#include <cstddef>
#include <sys/types.h>
#include <sys/stat.h>

namespace nlx::file
{

[[nodiscard]] bool mkdir_p(const char *path) noexcept;
[[nodiscard]] bool writable(const char *path) noexcept;
[[nodiscard]] ssize_t read_file(const char *path, char *buffer, size_t capacity) noexcept;

namespace detail
{

[[nodiscard]] ssize_t get_utf8bom_size(const char *data, size_t size) noexcept;

} // namespace nlx::file::detail
} // namespace nlx::file
#endif // !NLX_CORE_FILE_H
