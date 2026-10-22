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
    HttpInstruments,
    HttpCandles,
    HttpSetPositionMode,
    HttpSetAccountMode,
    WebsocketConnect,
    WebsocketPublicSubscribe,
    WebsocketPrivateSubscribe,
    OkxRequestsSize
};

namespace detail
{

template<typename Rule, size_t N>
consteval std::array<uint32_t, N + 1> offsets(const std::array<Rule, N> &rules) noexcept
{
    std::array<uint32_t, N + 1> result{};
    for (size_t index = 0; index < N; ++index)
    {
        result[index + 1] = result[index] + rules[index].count;
    }

    return result;
}

} // namespace detail

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
        { 60, seconds(2) },
        { 20, seconds(2) },
        { 40, seconds(2) },
        { 5, seconds(2) },
        { 5, seconds(2) },
        { 3, seconds(1) },
        { 480, minutes(60) },
        { 480, minutes(60) }
    }};

    static constexpr std::array<uint32_t, OkxRequestsSize + 1> kOffsets = detail::offsets(kRules);

    static constexpr size_t kCapacity = kOffsets[OkxRequestsSize];
    static constexpr nanoseconds kEmpty{ std::numeric_limits<nanoseconds::rep>::min() / 2 };

    std::mutex m_lock;
    std::array<nanoseconds, kCapacity> m_times;
    std::array<uint32_t, OkxRequestsSize> m_cursors;
};

} // namespace nlx::exchange
#endif // !NLX_EXCHANGE_OKX_RATE_LIMITER_H
