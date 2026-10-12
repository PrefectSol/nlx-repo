#ifndef NLX_CORE_TIME_H
#define NLX_CORE_TIME_H

#include <sys/time.h>
#include <chrono>
#include <format>

namespace nlx
{

using nanoseconds = std::chrono::nanoseconds;
using microseconds = std::chrono::microseconds;
using milliseconds = std::chrono::milliseconds;
using seconds = std::chrono::seconds;
using minutes = std::chrono::minutes;

namespace time
{

[[nodiscard]] nanoseconds now() noexcept;
[[nodiscard]] nanoseconds now_monotonic() noexcept;

} // namespace nlx::time

namespace detail
{

template<size_t N>
[[nodiscard]] std::string_view format_time(char (&buffer)[N], 
                                           std::format_string<const std::chrono::sys_time<nanoseconds> &> format,
                                           nanoseconds timestamp) noexcept;

[[nodiscard]] constexpr itimerspec to_itimerspec(milliseconds interval) noexcept;

} // namespace nlx::detail
} // namespace nlx

namespace nlx::detail
{

template<size_t N>
std::string_view format_time(char (&buffer)[N],
                             std::format_string<const std::chrono::sys_time<nanoseconds> &> format,
                             nanoseconds timestamp) noexcept
{
    const std::chrono::sys_time<std::chrono::nanoseconds> point(timestamp);

    const auto result = std::format_to_n(buffer, N - 1, format, point);
    *result.out = '\0';
    
    return { buffer, static_cast<size_t>(result.out - buffer) };
}

constexpr itimerspec to_itimerspec(milliseconds interval) noexcept
{
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(interval);
    const auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(interval - secs);

    itimerspec spec{};
    spec.it_value.tv_sec = secs.count();
    spec.it_value.tv_nsec = nsecs.count();
    spec.it_interval = spec.it_value;
    
    return spec;
}

} // namespace nlx::detail
#endif // !NLX_CORE_TIME_H

