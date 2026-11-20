#ifndef NLX_SERVICE_EXECUTOR_H
#define NLX_SERVICE_EXECUTOR_H

#include "core/market.h"
#include "core/metrics.h"
#include "core/time.h"
#include "logging/logger_node.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace nlx::service
{

enum class ExecutorCommandType : uint8_t
{
    Open,
    Close,
    Release
};

struct ExecutorCommand
{
    char instrument[Instrument::kIdCapacity];
    size_t slot;
    uint64_t episode;
    int64_t code;
    double lot;
    double tick;
    double size;
    double price;
    nanoseconds submitted;
    ExecutorCommandType type;
    PositionSide side;
    OrderType order;
};

enum class AccountEventType : uint8_t
{
    Balance,
    Disconnected,
    Filled,
    Rejected,
    Flat,
    Released
};

struct AccountEvent
{
    AccountEventType type;
    size_t slot;
    uint64_t episode;
    Balance balance;
    int64_t timestamp;
    double size;
    double price;
    int code;
    PositionSide side;
    bool closing;
};

template<typename Streamer>
class Executor
{
public:
    static constexpr size_t kSlotsCapacity = 3;
    static constexpr size_t kTicketsCapacity = 64;
    static constexpr size_t kHoldingsCapacity = 64;
    static constexpr size_t kKnownCapacity = 64;
    static constexpr size_t kEventsCapacity = 64;

    explicit Executor(Streamer &streamer, logging::LoggerNode &log, uint32_t tag) noexcept;

    Executor(const Executor &) = delete;
    Executor &operator=(const Executor &) = delete;
    Executor(Executor &&) = delete;
    Executor &operator=(Executor &&) = delete;

    void command(const ExecutorCommand &command, nanoseconds now) noexcept;
    void orders(std::span<const Order> orders, nanoseconds now) noexcept;
    void positions(std::span<const Position> positions, bool snapshot, nanoseconds now) noexcept;
    void reply(const OrderReply &reply, nanoseconds now) noexcept;
    void tick(nanoseconds now) noexcept;
    void drain(nanoseconds now) noexcept;
    void report() noexcept;

    [[nodiscard]] bool flat() const noexcept;
    [[nodiscard]] bool pop(AccountEvent &event) noexcept;

private:
    static constexpr size_t kNone = kSlotsCapacity;
    static constexpr size_t kRetryMilliseconds = 2000;
    static constexpr size_t kCancelMilliseconds = 1500;
    static constexpr size_t kReplyMilliseconds = 10000;
    static constexpr size_t kStuckMilliseconds = 10000;
    static constexpr size_t kSettleMilliseconds = 5000;

    struct Ticket
    {
        char client[Instrument::kIdCapacity];
        char id[Instrument::kIdCapacity];
        char instrument[Instrument::kIdCapacity];
        int64_t code;
        size_t slot;
        uint64_t episode;
        double size;
        double filled;
        double price;
        nanoseconds sent;
        nanoseconds cancelled;
        PositionSide side;
        bool closing;
        bool acknowledged;
        bool used;
    };

    struct Holding
    {
        char instrument[Instrument::kIdCapacity];
        double size;
        double price;
        PositionSide side;
        bool used;
    };

    struct Known
    {
        char instrument[Instrument::kIdCapacity];
        int64_t code;
        double lot;
    };

    struct Seat
    {
        char instrument[Instrument::kIdCapacity];
        uint64_t episode;
        int64_t code;
        double lot;
        double tick;
        std::array<double, 2> exposure;
        nanoseconds flattened;
        nanoseconds activity;
        size_t attempts;
        bool bound;
        bool closing;
        bool releasing;
    };

    Streamer &m_streamer;
    logging::LoggerNode &m_log;

    std::array<Ticket, kTicketsCapacity> m_tickets;
    std::array<Holding, kHoldingsCapacity> m_holdings;
    std::array<Known, kKnownCapacity> m_known;
    std::array<Seat, kSlotsCapacity> m_seats;
    std::array<AccountEvent, kEventsCapacity> m_events;

    size_t m_known_size;
    size_t m_events_head;
    size_t m_events_tail;

    uint32_t m_tag;
    uint64_t m_sequence;
    bool m_draining;

    LatencyHistogram m_order_trip;
    LatencyHistogram m_cancel_trip;
    LatencyHistogram m_gateway;
    LatencyHistogram m_fill;
    LatencyHistogram m_push;
    size_t m_rejects;

    void open(const ExecutorCommand &command, nanoseconds now) noexcept;
    void close(const ExecutorCommand &command, nanoseconds now) noexcept;

    void flatten(size_t slot, nanoseconds now) noexcept;
    void sweep(nanoseconds now) noexcept;
    void settle(size_t slot, nanoseconds now) noexcept;
    bool liquidate(size_t slot, const char *instrument, int64_t code, double lot, PositionSide side, double amount, nanoseconds now) noexcept;
    void withdraw(Ticket &ticket, nanoseconds now) noexcept;

    Ticket *allocate() noexcept;
    Ticket *find_ticket(std::string_view client) noexcept;
    Holding *find_holding(std::string_view instrument, PositionSide side) noexcept;
    const Known *find_known(std::string_view instrument) const noexcept;
    bool working(size_t slot, PositionSide side, bool closing) const noexcept;
    double held(std::string_view instrument, PositionSide side) const noexcept;

    void emit(AccountEventType type, size_t slot, uint64_t episode, double size, double price, int code, PositionSide side, bool closing) noexcept;
};

} // namespace nlx::service
#endif // !NLX_SERVICE_EXECUTOR_H
