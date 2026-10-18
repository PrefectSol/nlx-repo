#ifndef NLX_EXCHANGE_OKX_RATE_LIMITER_H
#define NLX_EXCHANGE_OKX_RATE_LIMITER_H

#include "core/time.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace nlx::exchange 
{

enum OkxRequest : size_t
{
    HttpTime,
    HttpAccountConfig,
    HttpBalance,
    HttpPositions,
    HttpPendingOrders,
    HttpSetPositionMode,
    HttpSetLeverage,
    WebsocketConnect,
    WebsocketPublicSubscribe,
    WebsocketPrivateSubscribe,
    OkxRequestsSize
};

namespace detail
{

template<typename Rule, size_t N>
consteval size_t total_count(const std::array<Rule, N> &rules) noexcept
{
    size_t count = 0;
    for (const Rule &rule : rules)
    {
        count += rule.count;
    }

    return count;
}

} // namespace nlx::exchange::detail

class OkxRateLimiter
{
public:
    struct Token
    {
        nanoseconds *request;
        nanoseconds *global;
    };

    OkxRateLimiter() noexcept;

    OkxRateLimiter(const OkxRateLimiter &) = delete;
    OkxRateLimiter &operator=(const OkxRateLimiter &) = delete;
    OkxRateLimiter(OkxRateLimiter &&) = delete;
    OkxRateLimiter &operator=(OkxRateLimiter &&) = delete;

    [[nodiscard]] nanoseconds acquire(OkxRequest request, nanoseconds now, Token &token) noexcept;
    void complete(const Token &token, nanoseconds now) noexcept;
    void penalize(OkxRequest request, nanoseconds now) noexcept;

private:
    enum Global : size_t
    {
        Orders,
        GlobalsSize,
        None = GlobalsSize
    };

    struct Rule
    {
        uint32_t count;
        nanoseconds window;
    };

    struct History
    {
        nanoseconds *times;
        uint32_t cursor;
    };

    static constexpr nanoseconds kPending = nanoseconds::max();

    static constexpr std::array<Rule, OkxRequestsSize> kRules = {{
        { 10, seconds(2) },
        { 5, seconds(2) },
        { 10, seconds(2) },
        { 10, seconds(2) },
        { 60, seconds(2) },
        { 5, seconds(2) },
        { 20, seconds(2) },
        { 3, seconds(1) },
        { 480, minutes(60) },
        { 480, minutes(60) }
    }};

    static constexpr std::array<Global, OkxRequestsSize> kGlobals = { 
        None, None, None, None, None, None, None, None, None, None 
    };

    static constexpr std::array<Rule, GlobalsSize> kGlobalRules = {{
        { 1000, seconds(2) }
    }};

    static constexpr size_t kTimesSize = detail::total_count(kRules) + detail::total_count(kGlobalRules);

    std::mutex m_lock;
    std::array<nanoseconds, kTimesSize> m_times;
    std::array<History, OkxRequestsSize> m_histories;
    std::array<History, GlobalsSize> m_global_histories;

    static nanoseconds delay(const Rule &rule, const History &history, nanoseconds now) noexcept;
    static nanoseconds *advance(const Rule &rule, History &history) noexcept;
};

} // namespace nlx::exchange
#endif // !NLX_EXCHANGE_OKX_RATE_LIMITER_H
