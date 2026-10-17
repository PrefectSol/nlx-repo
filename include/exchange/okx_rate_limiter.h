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
    HttpSetPositionMode,
    HttpSetLeverage,
    WebsocketConnect,
    WebsocketPublicSubscribe,
    WebsocketPrivateSubscribe,
    OkxRequestsSize
};

class OkxRateLimiter
{
public:
    OkxRateLimiter() noexcept;

    OkxRateLimiter(const OkxRateLimiter &) = delete;
    OkxRateLimiter &operator=(const OkxRateLimiter &) = delete;
    OkxRateLimiter(OkxRateLimiter &&) = delete;
    OkxRateLimiter &operator=(OkxRateLimiter &&) = delete;

    [[nodiscard]] nanoseconds acquire(OkxRequest request, nanoseconds now) noexcept;
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

    static constexpr std::array<Rule, OkxRequestsSize> kRules = {{
        { 10, seconds(2) },
        { 5, seconds(2) },
        { 10, seconds(2) },
        { 10, seconds(2) },
        { 5, seconds(2) },
        { 20, seconds(2) },
        { 3, seconds(1) },
        { 480, minutes(60) },
        { 480, minutes(60) }
    }};

    static constexpr std::array<Global, OkxRequestsSize> kGlobals = { 
        None, None, None, None, None, None, None, None, None 
    };

    static constexpr std::array<Rule, GlobalsSize> kGlobalRules = {{
        { 1000, seconds(2) }
    }};

    std::mutex m_lock;
    std::array<nanoseconds, OkxRequestsSize> m_ready;
    std::array<nanoseconds, GlobalsSize> m_global_ready;

    static nanoseconds delay(const Rule &rule, nanoseconds ready, nanoseconds now) noexcept;
    static nanoseconds advance(const Rule &rule, nanoseconds ready, nanoseconds now) noexcept;
};

} // namespace nlx::exchange
#endif // !NLX_EXCHANGE_OKX_RATE_LIMITER_H
