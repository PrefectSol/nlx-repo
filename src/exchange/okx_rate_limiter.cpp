#include "exchange/okx_rate_limiter.h"

#include <algorithm>

namespace nlx::exchange
{

namespace
{

template<typename Rule, size_t N>
consteval bool fits(const std::array<Rule, N> &rules, size_t capacity) noexcept
{
    for (const Rule &rule : rules)
    {
        if (rule.count == 0 || rule.count > capacity)
        {
            return false;
        }
    }

    return true;
}

} // namespace

OkxRateLimiter::OkxRateLimiter() noexcept :
    m_lock(),
    m_histories(),
    m_order_history()
{
    static_assert(fits(kRules, kHistoryCapacity));
    static_assert(kOrderRule.count != 0 && kOrderRule.count <= kHistoryCapacity);

    for (History &history : m_histories)
    {
        history.times.fill(kEmpty);
    }

    m_order_history.times.fill(kEmpty);
}

nanoseconds OkxRateLimiter::acquire(OkxRequest request, nanoseconds now, Ticket &ticket) noexcept
{
    const std::lock_guard<std::mutex> lock(m_lock);

    const Rule &rule = kRules[request];
    History &history = m_histories[request];

    nanoseconds wait = delay(rule, history, now);
    if (rule.order)
    {
        wait = std::max(wait, delay(kOrderRule, m_order_history, now));
    }

    if (wait != nanoseconds::zero())
    {
        return wait;
    }

    ticket.request = request;
    ticket.entry = reserve(rule, history);
    ticket.order_entry = kNoEntry;

    if (rule.order)
    {
        ticket.order_entry = reserve(kOrderRule, m_order_history);
    }

    return nanoseconds::zero();
}

void OkxRateLimiter::complete(const Ticket &ticket, nanoseconds now) noexcept
{
    const std::lock_guard<std::mutex> lock(m_lock);

    m_histories[ticket.request].times[ticket.entry] = now;

    if (ticket.order_entry != kNoEntry)
    {
        m_order_history.times[ticket.order_entry] = now;
    }
}

void OkxRateLimiter::penalize(OkxRequest request, nanoseconds now) noexcept
{
    const std::lock_guard<std::mutex> lock(m_lock);

    History &history = m_histories[request];
    for (uint32_t entry = 0; entry < kRules[request].count; ++entry)
    {
        history.times[entry] = std::max(history.times[entry], now);
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

uint32_t OkxRateLimiter::reserve(const Rule &rule, History &history) noexcept
{
    const uint32_t entry = history.cursor;

    history.times[entry] = kPending;
    history.cursor = (entry + 1) % rule.count;

    return entry;
}

} // namespace nlx::exchange
