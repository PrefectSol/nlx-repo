#include "exchange/okx_rate_limiter.h"

#include <algorithm>

namespace nlx::exchange
{

OkxRateLimiter::OkxRateLimiter() noexcept :
    m_lock(),
    m_ready(),
    m_global_ready()
{

}

nanoseconds OkxRateLimiter::acquire(OkxRequest request, nanoseconds now) noexcept
{
    const std::lock_guard<std::mutex> lock(m_lock);

    const Global global = kGlobals[request];

    nanoseconds wait = delay(kRules[request], m_ready[request], now);
    if (global != None)
    {
        wait = std::max(wait, delay(kGlobalRules[global], m_global_ready[global], now));
    }

    if (wait != nanoseconds::zero())
    {
        return wait;
    }

    m_ready[request] = advance(kRules[request], m_ready[request], now);
    if (global != None)
    {
        m_global_ready[global] = advance(kGlobalRules[global], m_global_ready[global], now);
    }

    return nanoseconds::zero();
}

void OkxRateLimiter::penalize(OkxRequest request, nanoseconds now) noexcept
{
    const std::lock_guard<std::mutex> lock(m_lock);

    const Rule &rule = kRules[request];
    m_ready[request] = std::max(m_ready[request], now + rule.window + rule.window - rule.window / rule.count);
}

nanoseconds OkxRateLimiter::delay(const Rule &rule, nanoseconds ready, nanoseconds now) noexcept
{
    const nanoseconds burst = rule.window - rule.window / rule.count;
    const nanoseconds wait = std::max(ready, now) - now - burst;

    return std::max(wait, nanoseconds::zero());
}

nanoseconds OkxRateLimiter::advance(const Rule &rule, nanoseconds ready, nanoseconds now) noexcept
{
    return std::max(ready, now) + rule.window / rule.count;
}

} // namespace nlx::exchange
