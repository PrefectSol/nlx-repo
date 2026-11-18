#ifndef NLX_EXCHANGE_OKX_MARKET_STREAMER_H
#define NLX_EXCHANGE_OKX_MARKET_STREAMER_H

#include "core/file_descriptor.h"
#include "core/market.h"
#include "core/order_book.h"
#include "core/time.h"
#include "exchange/okx_rate_limiter.h"
#include "logging/logger_node.h"
#include "network/websocket_client.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nlx::exchange
{

class OkxMarketStreamer
{
public:
    explicit OkxMarketStreamer(OkxRateLimiter &limiter, logging::LoggerNode &log);

    OkxMarketStreamer(const OkxMarketStreamer &) = delete;
    OkxMarketStreamer &operator=(const OkxMarketStreamer &) = delete;
    OkxMarketStreamer(OkxMarketStreamer &&) = delete;
    OkxMarketStreamer &operator=(OkxMarketStreamer &&) = delete;

    void initialize();

    [[nodiscard]] bool subscribe(size_t slot, std::string_view instrument) noexcept;
    void unsubscribe(size_t slot) noexcept;

    [[nodiscard]] bool poll(MarketUpdate &update) noexcept;

    void heartbeat() noexcept;
    void close() noexcept;

    [[nodiscard]] const FileDescriptor &fd() const noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] bool alive() const noexcept;
    [[nodiscard]] bool ready(size_t slot) const noexcept;
    [[nodiscard]] double bid(size_t slot) const noexcept;
    [[nodiscard]] double ask(size_t slot) const noexcept;

    [[nodiscard]] static constexpr size_t capacity() noexcept;

private:
    static constexpr char kUrl[] = "wss://ws.okx.com/ws/v5/public";
    static constexpr char kClock[] = "BTC-USDT-SWAP";

    static constexpr size_t kSlotsCapacity = 3;
    static constexpr size_t kBooksCapacity = kSlotsCapacity + 1;
    static constexpr size_t kConnectMilliseconds = 5000;
    static constexpr size_t kReadMilliseconds = 1000;
    static constexpr size_t kClockMilliseconds = 2000;
    static constexpr int64_t kPhaseMilliseconds = 20;
    static constexpr size_t kBackoffMilliseconds = 1000;
    static constexpr size_t kBackoffLimitMilliseconds = 5000;

    struct Book
    {
        char id[Instrument::kIdCapacity];
        OrderBook levels;
        int64_t sequence;
        size_t slot;
        bool active;
        bool ready;
    };

    logging::LoggerNode &m_log;
    OkxRateLimiter &m_limiter;

    FileDescriptor m_epoll;
    network::WebsocketClient m_client;

    std::array<Book, kBooksCapacity> m_books;
    std::array<size_t, kSlotsCapacity> m_slots;

    uint32_t m_failures;

    std::string_view m_trades;
    size_t m_trades_slot;

    nanoseconds m_clock_at;
    int64_t m_clock_phase;
    bool m_phase_warned;

    nanoseconds m_retry_at;
    milliseconds m_backoff;
    bool m_upgrade;

    bool reconnect() noexcept;
    bool connect() noexcept;
    void drop() noexcept;

    bool request(std::string_view operation, std::string_view instrument, bool books, bool trades) noexcept;
    bool send(size_t size) noexcept;
    bool read(std::string_view &text) noexcept;

    bool next_trade(MarketUpdate &update) noexcept;
    bool apply(size_t index, std::string_view body, MarketUpdate &update) noexcept;
    bool apply_levels(OrderBook &book, std::string_view levels, bool bids) noexcept;
    void resync(size_t index) noexcept;
    void dispatch(std::string_view text) noexcept;

    size_t find(std::string_view id) const noexcept;
    bool idle() const noexcept;
};

constexpr size_t OkxMarketStreamer::capacity() noexcept
{
    return kSlotsCapacity;
}

} // namespace nlx::exchange
#endif // !NLX_EXCHANGE_OKX_MARKET_STREAMER_H
