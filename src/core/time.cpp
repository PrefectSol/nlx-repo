#include "core/time.h"

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

} // namespace nlx::time
