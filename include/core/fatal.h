#ifndef NLX_CORE_FATAL_H
#define NLX_CORE_FATAL_H

#define NLX_STR_IMPL(x) #x
#define NLX_STR(x) NLX_STR_IMPL(x)
#define NLX_FATAL(msg) ::nlx::fatal::fatal("[FATAL] " msg "\n  at " __FILE__ ":" NLX_STR(__LINE__) "\n")

#include <cstddef>
#include <exception>
#include <cerrno>
#include <unistd.h>

namespace nlx::fatal
{

template<size_t N>
[[noreturn]] void fatal(const char (&message)[N]) noexcept;

} // namespace nlx::fatal

template<size_t N>
void nlx::fatal::fatal(const char (&message)[N]) noexcept
{
    static_assert(N > 1);

    size_t offset = 0;
    while (offset < N - 1)
    {
        ssize_t result;
        do
        {
            result = ::write(STDERR_FILENO, message + offset, N - 1 - offset);
        }
        while (result < 0 && errno == EINTR);

        if (result <= 0)
        {
            break;
        }

        offset += static_cast<size_t>(result);
    }

    std::terminate();
}

#endif // !NLX_CORE_FATAL_H

