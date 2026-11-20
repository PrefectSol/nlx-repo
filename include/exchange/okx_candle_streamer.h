#ifndef NLX_EXCHANGE_OKX_CANDLE_STREAMER_H
#define NLX_EXCHANGE_OKX_CANDLE_STREAMER_H

#include "core/file_descriptor.h"
#include "core/market.h"
#include "core/time.h"
#include "exchange/okx_rate_limiter.h"
#include "logging/logger_node.h"
#include "network/websocket_client.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace nlx::exchange
{

class OkxCandleStreamer
{
public:
    explicit OkxCandleStreamer(OkxRateLimiter &limiter, const char *url, logging::LoggerNode &log);

    OkxCandleStreamer(const OkxCandleStreamer &) = delete;
    OkxCandleStreamer &operator=(const OkxCandleStreamer &) = delete;
    OkxCandleStreamer(OkxCandleStreamer &&) = delete;
    OkxCandleStreamer &operator=(OkxCandleStreamer &&) = delete;

    void initialize();

    [[nodiscard]] bool resubscribe(std::span<const Instrument> instruments) noexcept;

    [[nodiscard]] bool poll(size_t &index) noexcept;
    [[nodiscard]] bool extract(Candle &candle) noexcept;

    void heartbeat() noexcept;
    void close() noexcept;

    [[nodiscard]] const FileDescriptor &fd() const noexcept;
    [[nodiscard]] std::string_view instrument(size_t index) const noexcept;
    [[nodiscard]] size_t size() const noexcept;

    [[nodiscard]] static constexpr size_t capacity() noexcept;

private:

    static constexpr size_t kInstrumentsCapacity = 1024;
    static constexpr size_t kConnectMilliseconds = 5000;
    static constexpr size_t kResponseMilliseconds = 1000;
    static constexpr size_t kReadMilliseconds = 1000;
    static constexpr size_t kHeartbeatMilliseconds = 15000;
    static constexpr size_t kBackoffMilliseconds = 1000;
    static constexpr size_t kBackoffLimitMilliseconds = 30000;

    static_assert(std::has_single_bit(kInstrumentsCapacity) && kInstrumentsCapacity < UINT16_MAX);

    struct Slot
    {
        char id[Instrument::kIdCapacity];
        bool subscribed;
    };

    logging::LoggerNode &m_log;
    OkxRateLimiter &m_limiter;
    const char *m_url;

    FileDescriptor m_epoll;
    network::WebsocketClient m_client;

    std::array<Slot, kInstrumentsCapacity> m_slots;
    std::array<uint16_t, kInstrumentsCapacity * 2> m_index;

    size_t m_size;
    size_t m_subscribed;

    nanoseconds m_received;
    nanoseconds m_pinged_at;
    bool m_pinged;

    nanoseconds m_retry_at;
    milliseconds m_backoff;
    bool m_upgrade;

    std::string_view m_body;

    bool reconnect() noexcept;
    bool subscribe() noexcept;
    bool connect() noexcept;
    void acquire(OkxRequest request) noexcept;

    bool send(size_t size) noexcept;
    bool read(std::string_view &text) noexcept;
    bool dispatch(std::string_view text) noexcept;

    size_t find(std::string_view id) const noexcept;
    void insert(size_t index) noexcept;
};

constexpr size_t OkxCandleStreamer::capacity() noexcept
{
    return kInstrumentsCapacity;
}

} // namespace nlx::exchange
#endif // !NLX_EXCHANGE_OKX_CANDLE_STREAMER_H
