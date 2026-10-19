#ifndef NLX_EXCHANGE_OKX_RATE_LIMITER_H
#define NLX_EXCHANGE_OKX_RATE_LIMITER_H

#include "core/time.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

namespace nlx::exchange 
{

enum OkxRequest : size_t
{
    HttpSystemTime,
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

class OkxRateLimiter
{
public:
    struct Ticket
    {
        OkxRequest request;
        uint32_t entry;
        uint32_t order_entry;
    };

    OkxRateLimiter() noexcept;

    OkxRateLimiter(const OkxRateLimiter &) = delete;
    OkxRateLimiter &operator=(const OkxRateLimiter &) = delete;
    OkxRateLimiter(OkxRateLimiter &&) = delete;
    OkxRateLimiter &operator=(OkxRateLimiter &&) = delete;

    [[nodiscard]] nanoseconds acquire(OkxRequest request, nanoseconds now, Ticket &ticket) noexcept;
    void complete(const Ticket &ticket, nanoseconds now) noexcept;
    void penalize(OkxRequest request, nanoseconds now) noexcept;

private:
    static constexpr size_t kHistoryCapacity = 1000;
    static constexpr uint32_t kNoEntry = std::numeric_limits<uint32_t>::max();

    static constexpr nanoseconds kEmpty = nanoseconds::min();
    static constexpr nanoseconds kPending = nanoseconds::max();

    struct Rule
    {
        uint32_t count;
        nanoseconds window;
        bool order;
    };

    struct History
    {
        std::array<nanoseconds, kHistoryCapacity> times;
        uint32_t cursor;
    };

    static constexpr std::array<Rule, OkxRequestsSize> kRules = {{
        { 10, seconds(2), false },
        { 5, seconds(2), false },
        { 10, seconds(2), false },
        { 10, seconds(2), false },
        { 60, seconds(2), false },
        { 5, seconds(2), false },
        { 20, seconds(2), false },
        { 3, seconds(1), false },
        { 480, minutes(60), false },
        { 480, minutes(60), false }
    }};

    static constexpr Rule kOrderRule = { 1000, seconds(2), false };

    std::mutex m_lock;
    std::array<History, OkxRequestsSize> m_histories;
    History m_order_history;

    static nanoseconds delay(const Rule &rule, const History &history, nanoseconds now) noexcept;
    static uint32_t reserve(const Rule &rule, History &history) noexcept;
};

} // namespace nlx::exchange
#endif // !NLX_EXCHANGE_OKX_RATE_LIMITER_H
