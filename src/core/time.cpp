#include "core/time.h"

#include <sys/timex.h>

namespace nlx::time
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
