#include "exchange/okx_rate_limiter.h"

#include <algorithm>

namespace nlx::exchange
{

OkxRateLimiter::OkxRateLimiter() noexcept :
    m_lock(),
    m_times(),
    m_histories(),
    m_global_histories()
{
    m_times.fill(nanoseconds::min());

    nanoseconds *times = m_times.data();
    for (size_t index = 0; index < OkxRequestsSize; ++index)
    {
        m_histories[index] = { times, 0 };
        times += kRules[index].count;
    }

    for (size_t index = 0; index < GlobalsSize; ++index)
    {
        m_global_histories[index] = { times, 0 };
        times += kGlobalRules[index].count;
    }
}

nanoseconds OkxRateLimiter::acquire(OkxRequest request, nanoseconds now, Token &token) noexcept
{
    const std::lock_guard<std::mutex> lock(m_lock);

    const Global global = kGlobals[request];

    nanoseconds wait = delay(kRules[request], m_histories[request], now);
    if (global != None)
    {
        wait = std::max(wait, delay(kGlobalRules[global], m_global_histories[global], now));
    }

    if (wait != nanoseconds::zero())
    {
        return wait;
    }

    token.request = advance(kRules[request], m_histories[request]);
    token.global = nullptr;

    if (global != None)
    {
        token.global = advance(kGlobalRules[global], m_global_histories[global]);
    }

    return nanoseconds::zero();
}

void OkxRateLimiter::complete(const Token &token, nanoseconds now) noexcept
{
    const std::lock_guard<std::mutex> lock(m_lock);

    *token.request = now;
    if (token.global != nullptr)
    {
        *token.global = now;
    }
}

void OkxRateLimiter::penalize(OkxRequest request, nanoseconds now) noexcept
{
    const std::lock_guard<std::mutex> lock(m_lock);

    const History &history = m_histories[request];
    for (uint32_t index = 0; index < kRules[request].count; ++index)
    {
        history.times[index] = std::max(history.times[index], now);
    }
}

nanoseconds OkxRateLimiter::delay(const Rule &rule, const History &history, nanoseconds now) noexcept
{
    const nanoseconds oldest = history.times[history.cursor];
    if (oldest == kPending)
    {
        return kPending;
    }

    if (oldest <= now - rule.window)
    {
        return nanoseconds::zero();
    }

    return oldest + rule.window - now;
}

nanoseconds *OkxRateLimiter::advance(const Rule &rule, History &history) noexcept
{
    nanoseconds *entry = history.times + history.cursor;
    *entry = kPending;

    ++history.cursor;
    if (history.cursor == rule.count)
    {
        history.cursor = 0;
    }

    return entry;
}

} // namespace nlx::exchange
