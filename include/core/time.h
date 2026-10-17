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

template <size_t Width>
[[nodiscard]] constexpr char *write_padded(char *data, std::unsigned_integral auto value) noexcept;

template<size_t N>
[[nodiscard]] std::string_view format_time(char (&buffer)[N], 
                                           std::format_string<const std::chrono::sys_time<nanoseconds> &> format,
                                           nanoseconds timestamp) noexcept;

[[nodiscard]] constexpr std::array<char, time::iso8601_ms_size> format_iso8601_ms(std::chrono::nanoseconds timestamp) noexcept;

[[nodiscard]] constexpr itimerspec to_itimerspec(milliseconds interval) noexcept;
[[nodiscard]] constexpr milliseconds to_milliseconds(nanoseconds interval) noexcept;

} // namespace nlx::detail
} // namespace nlx

namespace nlx::detail
{

template <size_t Width>
constexpr char *write_padded(char *data, std::unsigned_integral auto value) noexcept
{
    for (size_t i = Width; i > 0; --i) 
    {
        data[i - 1] = static_cast<char>('0' + value % 10);
        value /= 10;
    }

    return data + Width;
}

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

constexpr std::array<char, time::iso8601_ms_size> format_iso8601_ms(std::chrono::nanoseconds timestamp) noexcept
{
    static_assert(time::iso8601_ms_size == 25);

    const std::chrono::sys_time<std::chrono::milliseconds> point{std::chrono::floor<std::chrono::milliseconds>(timestamp)};
    const std::chrono::sys_days day_point = std::chrono::floor<std::chrono::days>(point);

    const std::chrono::year_month_day ymd{day_point};
    const std::chrono::hh_mm_ss hms{point - day_point};

    std::array<char, time::iso8601_ms_size> buffer{};
    char *ptr = buffer.data();
    
    ptr = detail::write_padded<4>(ptr, static_cast<unsigned>(static_cast<int>(ymd.year())));
    *ptr++ = '-';
    ptr = detail::write_padded<2>(ptr, static_cast<unsigned>(ymd.month()));
    *ptr++ = '-';
    ptr = detail::write_padded<2>(ptr, static_cast<unsigned>(ymd.day()));
    *ptr++ = 'T';
    ptr = detail::write_padded<2>(ptr, static_cast<unsigned>(hms.hours().count()));
    *ptr++ = ':';
    ptr = detail::write_padded<2>(ptr, static_cast<unsigned>(hms.minutes().count()));
    *ptr++ = ':';
    ptr = detail::write_padded<2>(ptr, static_cast<unsigned>(hms.seconds().count()));
    *ptr++ = '.';
    ptr = detail::write_padded<3>(ptr, static_cast<unsigned>(hms.subseconds().count()));
    *ptr++ = 'Z';
    *ptr = '\0';

    return buffer;
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

