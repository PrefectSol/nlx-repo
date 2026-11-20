#ifndef NLX_SERVICE_MARKET_H
#define NLX_SERVICE_MARKET_H

#include "core/file_descriptor.h"
#include "core/market.h"
#include "core/metrics.h"
#include "core/spsc_queue.h"
#include "core/time.h"
#include "exchange/okx_rate_limiter.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nlx::service
{

enum class MarketEventType : uint8_t
{
    Bar,
    Failure
};

struct MarketEvent
{
    MarketEventType type;
    size_t slot;
    uint64_t episode;
    Bar bar;
};

template<typename Streamer>
class Market
{
public:
    explicit Market(exchange::OkxRateLimiter &limiter, const char *url, size_t clock_milliseconds, logging::LoggerRegistry &registry);

    Market(const Market &) = delete;
    Market &operator=(const Market &) = delete;
    Market(Market &&) = delete;
    Market &operator=(Market &&) = delete;

    void initialize();
    void start() noexcept;
    void request_stop() noexcept;

    [[nodiscard]] bool subscribe(size_t slot, uint64_t episode, std::string_view instrument, double contract) noexcept;
    [[nodiscard]] bool unsubscribe(size_t slot, uint64_t episode) noexcept;

    [[nodiscard]] const FileDescriptor &events() const noexcept;
    [[nodiscard]] bool pop(MarketEvent &event) noexcept;
    [[nodiscard]] bool ready() const noexcept;

    [[nodiscard]] static constexpr size_t capacity() noexcept;

private:
    static constexpr size_t kTimerMilliseconds = 1000;
    static constexpr size_t kReadyMilliseconds = 5000;
    static constexpr size_t kCommandsCapacity = 16;
    static constexpr size_t kEventsCapacity = 1024;
    static constexpr int64_t kGapSeconds = 60;
    static constexpr size_t kMetricsTicks = 60;

    struct Command
    {
        char instrument[Instrument::kIdCapacity];
        size_t slot;
        uint64_t episode;
        double contract;
        bool subscribe;
    };

    struct Slot
    {
        char instrument[Instrument::kIdCapacity];
        uint64_t episode;
        double contract;

        Bar bar;
        double bid;
        double ask;
        double buy_limit;
        double sell_limit;

        int64_t first;
        size_t bars;
        size_t late;
        size_t reported;

        nanoseconds started;

        bool traded;
        bool priced;
        bool active;
    };

    logging::LoggerNode m_log;
    Streamer m_streamer;

    FileDescriptor m_epoll;
    FileDescriptor m_stop;
    FileDescriptor m_command;
    FileDescriptor m_timer;
    FileDescriptor m_event;

    std::atomic<bool> m_initialized;
    std::atomic<bool> m_ready;

    SpscQueue<Command, kCommandsCapacity> m_commands;
    SpscQueue<MarketEvent, kEventsCapacity> m_events;

    std::array<Slot, Streamer::capacity()> m_slots;

    int64_t m_second;
    size_t m_ticks;

    LoopMeter m_loop;
    LatencyHistogram m_receive;
    LatencyHistogram m_delivery;
    LatencyHistogram m_close;
    size_t m_updates;

    void command() noexcept;
    void tick() noexcept;
    void receive() noexcept;
    void synchronize() noexcept;

    void open(const Command &command) noexcept;
    void release(size_t index, std::string_view reason) noexcept;
    void fail(size_t index, std::string_view reason) noexcept;

    void advance(int64_t timestamp) noexcept;
    void finish(int64_t second) noexcept;
    void quote(size_t index, int64_t timestamp) noexcept;
    void trade(Slot &slot, const MarketUpdate &update) noexcept;

    void emit(MarketEventType type, size_t index) noexcept;
    void report() noexcept;
};

template<typename Streamer>
constexpr size_t Market<Streamer>::capacity() noexcept
{
    return Streamer::capacity();
}

} // namespace nlx::service
#endif // !NLX_SERVICE_MARKET_H
