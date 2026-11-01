#ifndef NLX_SERVICE_SCREENER_H
#define NLX_SERVICE_SCREENER_H

#include "core/config.h"
#include "core/file_descriptor.h"
#include "core/sliding_window.h"
#include "core/spsc_queue.h"
#include "core/time.h"
#include "exchange/okx_rate_limiter.h"
#include "exchange/okx_streamer.h"
#include "exchange/okx_types.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace nlx::service
{

enum class ScreenerEvent : uint8_t
{
    Trigger,
    Cascade
};

struct ScreenerSignal
{
    char id[exchange::OkxInstrument::kIdCapacity];
    milliseconds time;
    size_t instruments;
    ScreenerEvent event;
};

struct ScreenerRequest
{
    std::span<const exchange::OkxInstrument> instruments;
    size_t ignored;
    bool connected;
    const FileDescriptor &notify;
};

class Screener
{
public:
    explicit Screener(const OkxConfig &config, exchange::OkxRateLimiter &limiter, logging::LoggerRegistry &registry);

    Screener(const Screener &) = delete;
    Screener &operator=(const Screener &) = delete;
    Screener(Screener &&) = delete;
    Screener &operator=(Screener &&) = delete;

    void initialize();
    void start() noexcept;
    void request_stop() noexcept;

    [[nodiscard]] bool resubscribe(ScreenerRequest &request) noexcept;
    [[nodiscard]] bool poll(ScreenerSignal &signal) noexcept;
    [[nodiscard]] const FileDescriptor &signals() const noexcept;

    static constexpr size_t kInstrumentsCapacity = exchange::OkxStreamer::kInstrumentsCapacity;
    static constexpr size_t kHistoryCapacity = 120;
    static constexpr size_t kWarmupBars = 64;

private:
    using History = SlidingWindow<exchange::OkxBar, kHistoryCapacity>;

    struct Track
    {
        milliseconds triggered;
        bool pending;
    };

    static constexpr size_t kSignalsCapacity = 256;
    static constexpr size_t kCascadeThreshold = 5;

    static constexpr milliseconds kTickInterval{ 1000 };
    static constexpr milliseconds kMinuteDeadline{ 2500 };
    static constexpr milliseconds kBarInterval = minutes(1);
    static constexpr milliseconds kTriggerCooldown = minutes(60);
    static constexpr milliseconds kCascadeWindow = minutes(5);
    static constexpr milliseconds kCascadeCooldown = minutes(60);
    static constexpr milliseconds kNever{ std::numeric_limits<milliseconds::rep>::min() / 2 };

    static_assert(kWarmupBars <= kHistoryCapacity);
    static_assert(kCascadeWindow <= kTriggerCooldown);

    logging::LoggerNode m_log;

    FileDescriptor m_epoll;
    FileDescriptor m_stop;
    FileDescriptor m_update;
    FileDescriptor m_timer;
    FileDescriptor m_deadline;
    FileDescriptor m_signal;

    std::atomic<bool> m_initialized;
    std::atomic<ScreenerRequest *> m_request;

    exchange::OkxStreamer m_streamer;
    SpscQueue<ScreenerSignal, kSignalsCapacity> m_signals;

    std::array<History, kInstrumentsCapacity> m_histories;
    std::array<Track, kInstrumentsCapacity> m_tracks;
    std::array<uint16_t, exchange::OkxStreamer::kTouchedCapacity> m_touched;

    milliseconds m_minute;
    milliseconds m_frozen;
    nanoseconds m_opened;
    nanoseconds m_compute;
    nanoseconds m_slowest;

    size_t m_expected;
    size_t m_arrived;
    size_t m_received;
    size_t m_detected;
    bool m_open;

    static void deliver(void *object, size_t index, const exchange::OkxBar &bar) noexcept;

    void update() noexcept;
    void reset(size_t index) noexcept;

    void accept(size_t index, const exchange::OkxBar &bar) noexcept;
    void open(milliseconds minute) noexcept;
    void evaluate(size_t index) noexcept;
    void complete(std::string_view reason) noexcept;
    bool emit(ScreenerEvent event, std::string_view id, size_t instruments) noexcept;

    [[nodiscard]] bool detect(std::span<const exchange::OkxBar> bars) const noexcept;
};

} // namespace nlx::service
#endif // !NLX_SERVICE_SCREENER_H
