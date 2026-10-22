#include "exchange/okx_rate_limiter.h"

#include <algorithm>

namespace nlx::exchange
{

OkxRateLimiter::OkxRateLimiter() noexcept :
    m_lock(),
    m_times(),
    m_cursors()
{
    m_times.fill(kEmpty);
}

nanoseconds OkxRateLimiter::acquire(OkxRequest request, nanoseconds now) noexcept
{
    const std::lock_guard<std::mutex> lock(m_lock);

    const Rule &rule = kRules[request];
    uint32_t &cursor = m_cursors[request];
    nanoseconds &oldest = m_times[kOffsets[request] + cursor];

    const nanoseconds ready = oldest + rule.window;
    if (ready > now)
    {
        return ready - now;
    }

    oldest = now;
    cursor = cursor + 1 == rule.count ? 0 : cursor + 1;

    return nanoseconds::zero();
}

void OkxRateLimiter::penalize(OkxRequest request, nanoseconds now) noexcept
{
    const std::lock_guard<std::mutex> lock(m_lock);

    const auto begin = m_times.begin() + kOffsets[request];
    const auto end = m_times.begin() + kOffsets[request + 1];

    for (auto it = begin; it != end; ++it)
    {
        *it = std::max(*it, now);
    }
}

} // namespace nlx::exchange
