#ifndef NLX_CORE_FILE_H
#define NLX_CORE_FILE_H

#include <sys/stat.h>

namespace nlx::file
{

[[nodiscard]] bool mkdir_p(const char *path) noexcept;
[[nodiscard]] bool writable(const char *path) noexcept;

} // namespace nlx::file
#endif // !NLX_CORE_FILE_H
