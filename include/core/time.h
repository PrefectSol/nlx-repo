#ifndef NLX_CORE_TIME_H
#define NLX_CORE_TIME_H

#include <sys/time.h>
#include <chrono>
#include <format>
#include <array>

namespace nlx
{

using nanoseconds = std::chrono::nanoseconds;
using microseconds = std::chrono::microseconds;
using milliseconds = std::chrono::milliseconds;
using seconds = std::chrono::seconds;
using minutes = std::chrono::minutes;

namespace time
{

inline constexpr size_t iso8601_ms_size = sizeof("YYYY-MM-DDTHH:MM:SS.mmmZ");

[[nodiscard]] nanoseconds now() noexcept;
[[nodiscard]] nanoseconds now_monotonic() noexcept;

[[nodiscard]] bool synchronized() noexcept;

} // namespace nlx::time

namespace detail
{

template<size_t N>
[[nodiscard]] std::string_view format_time(char (&buffer)[N], 
                                           std::format_string<const std::chrono::sys_time<nanoseconds> &> format,
                                           nanoseconds timestamp) noexcept;

[[nodiscard]] std::array<char, time::iso8601_ms_size> format_iso8601_ms(nanoseconds timestamp) noexcept;

[[nodiscard]] constexpr itimerspec to_itimerspec(milliseconds interval) noexcept;
[[nodiscard]] constexpr milliseconds to_milliseconds(nanoseconds interval) noexcept;

} // namespace nlx::detail
} // namespace nlx

namespace nlx::detail
{

template<size_t N>
std::string_view format_time(char (&buffer)[N],
                             std::format_string<const std::chrono::sys_time<nanoseconds> &> format,
                             nanoseconds timestamp) noexcept
{
    static_assert(N > 1);

    const std::chrono::sys_time<std::chrono::nanoseconds> point(timestamp);

    const auto result = std::format_to_n(buffer, N - 1, format, point);
    *result.out = '\0';
    
    return { buffer, static_cast<size_t>(result.out - buffer) };
}

constexpr itimerspec to_itimerspec(milliseconds interval) noexcept
{
    const std::chrono::seconds secs = std::chrono::duration_cast<std::chrono::seconds>(interval);
    const std::chrono::nanoseconds nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(interval - secs);

    itimerspec spec{};
    spec.it_value.tv_sec = secs.count();
    spec.it_value.tv_nsec = nsecs.count();
    spec.it_interval = spec.it_value;
    
    return spec;
}

constexpr milliseconds to_milliseconds(nanoseconds interval) noexcept
{
    return std::chrono::ceil<milliseconds>(interval);
}

} // namespace nlx::detail
#endif // !NLX_CORE_TIME_H
