#ifndef NLX_SERVICE_SCREENER_H
#define NLX_SERVICE_SCREENER_H

#include "core/file_descriptor.h"
#include "core/market.h"
#include "core/metrics.h"
#include "core/ring_buffer.h"
#include "core/spsc_queue.h"
#include "exchange/okx_rate_limiter.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace nlx::service
{

struct ScreenerSignal
{
    char instrument[Instrument::kIdCapacity];
    int64_t timestamp;
    double close;
    double impulse;
    double baseline;
    uint64_t generation;
};

template<typename Streamer>
class Screener
{
public:
    static constexpr size_t kWindowCandles = 4;
    static constexpr size_t kBaselineCandles = 60;
    static constexpr size_t kFilterCandles = kWindowCandles + kBaselineCandles;
    static constexpr size_t kCooldownCandles = 90;

    static constexpr double kImpulseThreshold = 0.03;
    static constexpr double kImpulseRatio = 5.0;

    explicit Screener(exchange::OkxRateLimiter &limiter, const char *url, logging::LoggerRegistry &registry);

    Screener(const Screener &) = delete;
    Screener &operator=(const Screener &) = delete;
    Screener(Screener &&) = delete;
    Screener &operator=(Screener &&) = delete;

    void initialize();
    void start() noexcept;
    void request_stop() noexcept;

    [[nodiscard]] bool resubscribe(std::span<const Instrument> instruments) noexcept;

    void cooldown() noexcept;

    [[nodiscard]] const FileDescriptor &signals() const noexcept;
    [[nodiscard]] bool pop(ScreenerSignal &signal) noexcept;
    [[nodiscard]] size_t discard() noexcept;

    [[nodiscard]] static bool filter(std::span<const Candle, kFilterCandles> candles, double &impulse, double &baseline) noexcept;

private:
    static constexpr size_t kTimerMilliseconds = 1000;
    static constexpr size_t kStatusTicks = 60;
    static constexpr size_t kSignalsCapacity = 256;
    static constexpr int64_t kCandleMilliseconds = 60000;

    static_assert(kWindowCandles >= 2);
    static_assert(kBaselineCandles >= kWindowCandles);
    static_assert(kCooldownCandles >= kFilterCandles);

    using Ring = RingBuffer<Candle, kCooldownCandles>;

    struct Request
    {
        std::span<const Instrument> instruments;
        bool result;
    };

    logging::LoggerNode m_log;
    Streamer m_streamer;

    FileDescriptor m_epoll;
    FileDescriptor m_stop;
    FileDescriptor m_update;
    FileDescriptor m_done;
    FileDescriptor m_timer;
    FileDescriptor m_signal;

    std::atomic<bool> m_initialized;
    std::atomic<Request *> m_request;
    std::atomic<uint64_t> m_cooldown;

    SpscQueue<ScreenerSignal, kSignalsCapacity> m_signals;
    std::array<Ring, Streamer::capacity()> m_rings;

    uint64_t m_generation;
    size_t m_ticks;

    size_t m_gaps;
    size_t m_gap_index;
    int64_t m_gap_candles;
    size_t m_stale;

    LoopMeter m_loop;
    LatencyHistogram m_receive;

    void update() noexcept;
    void tick() noexcept;
    void receive() noexcept;

    void accept(size_t index) noexcept;
    void notify(size_t index, const Candle &candle, double impulse, double baseline) noexcept;

    void synchronize() noexcept;
    void reset() noexcept;

    [[nodiscard]] static double change(const Candle &last, const Candle &first) noexcept;
};

} // namespace nlx::service

namespace nlx::service
{

template<typename Streamer>
bool Screener<Streamer>::filter(std::span<const Candle, kFilterCandles> candles, double &impulse, double &baseline) noexcept
{
    constexpr size_t step = kWindowCandles - 1;
    constexpr size_t windows = kBaselineCandles - step;

    impulse = change(candles[kFilterCandles - 1], candles[kFilterCandles - kWindowCandles]);
    baseline = 0.0;

    if (impulse <= kImpulseThreshold) [[likely]]
    {
        return false;
    }

    double sum = 0.0;
    for (size_t index = step; index < kBaselineCandles; ++index)
    {
        sum += change(candles[index], candles[index - step]);
    }

    baseline = sum / static_cast<double>(windows);

    return baseline * kImpulseRatio < impulse;
}

template<typename Streamer>
double Screener<Streamer>::change(const Candle &last, const Candle &first) noexcept
{
    return std::abs(last.close - first.close) / first.close;
}

} // namespace nlx::service
#endif // !NLX_SERVICE_SCREENER_H
