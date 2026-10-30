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
using hours = std::chrono::hours;
using days = std::chrono::days;

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

[[nodiscard]] constexpr timespec to_timespec(nanoseconds value) noexcept;
[[nodiscard]] constexpr itimerspec to_itimerspec(milliseconds interval) noexcept;
[[nodiscard]] constexpr itimerspec to_itimerspec(milliseconds interval, milliseconds delay) noexcept;

[[nodiscard]] constexpr milliseconds ceil_milliseconds(nanoseconds interval) noexcept;
[[nodiscard]] constexpr minutes ceil_minutes(milliseconds interval) noexcept;
[[nodiscard]] constexpr seconds floor_seconds(nanoseconds interval) noexcept;
[[nodiscard]] constexpr days floor_days(nanoseconds interval) noexcept;

[[nodiscard]] constexpr double ms(nanoseconds value) noexcept;

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

constexpr timespec to_timespec(nanoseconds value) noexcept
{
    const std::chrono::seconds secs = std::chrono::duration_cast<std::chrono::seconds>(value);
    const std::chrono::nanoseconds nsecs = value - secs;

    timespec spec{};
    spec.tv_sec = secs.count();
    spec.tv_nsec = nsecs.count();

    return spec;
}

constexpr itimerspec to_itimerspec(milliseconds interval) noexcept
{
    return to_itimerspec(interval, interval);
}

constexpr itimerspec to_itimerspec(milliseconds interval, milliseconds delay) noexcept
{
    const nanoseconds first = delay > milliseconds::zero() ? nanoseconds(delay) : nanoseconds(1);

    itimerspec spec{};
    spec.it_value = to_timespec(first);
    spec.it_interval = to_timespec(interval);

    return spec;
}

constexpr milliseconds ceil_milliseconds(nanoseconds interval) noexcept
{
    return std::chrono::ceil<milliseconds>(interval);
}

constexpr minutes ceil_minutes(milliseconds interval) noexcept
{
    return std::chrono::ceil<minutes>(interval);
}

constexpr seconds floor_seconds(nanoseconds interval) noexcept
{
    return std::chrono::floor<seconds>(interval);
}

constexpr days floor_days(nanoseconds interval) noexcept
{
    return std::chrono::floor<days>(interval);
}

constexpr double ms(nanoseconds value) noexcept
{
    return static_cast<double>(value.count()) / 1000000.0;
}

} // namespace nlx::detail
#endif // !NLX_CORE_TIME_H
