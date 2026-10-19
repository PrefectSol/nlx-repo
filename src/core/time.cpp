#include "core/time.h"

#include <sys/timex.h>

namespace nlx
{

namespace
{

template<size_t Width>
char *write_digits(char *data, unsigned value) noexcept
{
    static_assert(Width > 0);

    for (size_t i = Width; i > 0; --i)
    {
        data[i - 1] = static_cast<char>('0' + value % 10);
        value /= 10;
    }

    return data + Width;
}

} // namespace

namespace time
{

nanoseconds now() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch());
}

nanoseconds now_monotonic() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch());
}

bool synchronized() noexcept
{
    timex state{};
    const int result = ::ntp_adjtime(&state);

    return result >= 0 && result != TIME_ERROR && (state.status & STA_UNSYNC) == 0;
}

} // namespace nlx::time

namespace detail
{

std::array<char, time::iso8601_ms_size> format_iso8601_ms(nanoseconds timestamp) noexcept
{
    static_assert(time::iso8601_ms_size == 25);

    const std::chrono::sys_time<std::chrono::milliseconds> point{std::chrono::floor<std::chrono::milliseconds>(timestamp)};
    const std::chrono::sys_days day_point = std::chrono::floor<std::chrono::days>(point);

    const std::chrono::year_month_day ymd{day_point};
    const std::chrono::hh_mm_ss hms{point - day_point};

    std::array<char, time::iso8601_ms_size> buffer{};
    char *ptr = buffer.data();
    
    ptr = write_digits<4>(ptr, static_cast<unsigned>(static_cast<int>(ymd.year())));
    *ptr++ = '-';
    ptr = write_digits<2>(ptr, static_cast<unsigned>(ymd.month()));
    *ptr++ = '-';
    ptr = write_digits<2>(ptr, static_cast<unsigned>(ymd.day()));
    *ptr++ = 'T';
    ptr = write_digits<2>(ptr, static_cast<unsigned>(hms.hours().count()));
    *ptr++ = ':';
    ptr = write_digits<2>(ptr, static_cast<unsigned>(hms.minutes().count()));
    *ptr++ = ':';
    ptr = write_digits<2>(ptr, static_cast<unsigned>(hms.seconds().count()));
    *ptr++ = '.';
    ptr = write_digits<3>(ptr, static_cast<unsigned>(hms.subseconds().count()));
    *ptr++ = 'Z';
    *ptr = '\0';

    return buffer;
}

} // namespace nlx::detail
} // namespace nlx
