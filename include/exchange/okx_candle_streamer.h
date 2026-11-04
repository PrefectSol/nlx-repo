#ifndef NLX_EXCHANGE_OKX_CANDLE_STREAMER_H
#define NLX_EXCHANGE_OKX_CANDLE_STREAMER_H

#include "core/file_descriptor.h"
#include "core/time.h"
#include "exchange/okx_rate_limiter.h"
#include "exchange/okx_types.h"
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
    explicit OkxCandleStreamer(OkxRateLimiter &limiter, logging::LoggerNode &log);

    OkxCandleStreamer(const OkxCandleStreamer &) = delete;
    OkxCandleStreamer &operator=(const OkxCandleStreamer &) = delete;
    OkxCandleStreamer(OkxCandleStreamer &&) = delete;
    OkxCandleStreamer &operator=(OkxCandleStreamer &&) = delete;

    void initialize();

    [[nodiscard]] bool resubscribe(std::span<const OkxInstrument> instruments) noexcept;

    void receive() noexcept;
    void heartbeat() noexcept;
    void close() noexcept;

    [[nodiscard]] const FileDescriptor &fd() const noexcept;

private:
    static constexpr char kUrl[] = "wss://ws.okx.com/ws/v5/business";

    static constexpr size_t kInstrumentsCapacity = 1024;
    static constexpr size_t kConnectMilliseconds = 5000;
    static constexpr size_t kResponseMilliseconds = 1000;
    static constexpr size_t kHeartbeatMilliseconds = 15000;

    static_assert(std::has_single_bit(kInstrumentsCapacity) && kInstrumentsCapacity < UINT16_MAX);

    struct Slot
    {
        char id[OkxInstrument::kIdCapacity];
        bool subscribed;
    };

    logging::LoggerNode &m_log;
    OkxRateLimiter &m_limiter;

    FileDescriptor m_epoll;
    network::WebsocketClient m_client;

    std::array<Slot, kInstrumentsCapacity> m_slots;
    std::array<uint16_t, kInstrumentsCapacity * 2> m_index;

    size_t m_size;
    size_t m_subscribed;

    nanoseconds m_received;
    bool m_pinged;

    bool subscribe() noexcept;
    bool connect() noexcept;
    void acquire(OkxRequest request) noexcept;

    bool send(size_t size) noexcept;
    bool read(std::string_view &text) noexcept;
    bool dispatch(std::string_view text) noexcept;

    size_t find(std::string_view id) const noexcept;
    void insert(size_t index) noexcept;
};

} // namespace nlx::exchange
#endif // !NLX_EXCHANGE_OKX_CANDLE_STREAMER_H
