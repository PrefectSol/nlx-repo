#ifndef NLX_EXCHANGE_OKX_CANDLE_STREAMER_H
#define NLX_EXCHANGE_OKX_CANDLE_STREAMER_H

#include "core/file_descriptor.h"
#include "core/time.h"
#include "exchange/okx_rate_limiter.h"
#include "exchange/okx_types.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"
#include "network/websocket_client.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace nlx::exchange
{

class OkxCandleStreamer
{
public:
    explicit OkxCandleStreamer(OkxRateLimiter &limiter, logging::LoggerRegistry &registry);

    OkxCandleStreamer(const OkxCandleStreamer &) = delete;
    OkxCandleStreamer &operator=(const OkxCandleStreamer &) = delete;
    OkxCandleStreamer(OkxCandleStreamer &&) = delete;
    OkxCandleStreamer &operator=(OkxCandleStreamer &&) = delete;

    void initialize(const FileDescriptor &epoll);

    void tick(nanoseconds now) noexcept;
    void receive() noexcept;
    void close() noexcept;

    [[nodiscard]] size_t resubscribe(std::span<const std::string_view> instruments) noexcept;

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] int fd() const noexcept;

    static constexpr size_t kInstrumentsCapacity = 1024;

private:
    static constexpr char kUrl[] = "wss://ws.okx.com/ws/v5/business";

    static constexpr size_t kIdCapacity = OkxInstrument::kIdCapacity;

    static constexpr milliseconds kConnectTimeout{ 5000 };
    static constexpr milliseconds kReadTimeout{ 5000 };

    static constexpr nanoseconds kPingInterval = seconds(20);
    static constexpr nanoseconds kPongTimeout = seconds(10);
    static constexpr nanoseconds kReconnectDelay = seconds(1);
    static constexpr nanoseconds kReconnectLimit = seconds(60);
    static constexpr nanoseconds kStableInterval = seconds(60);
    static constexpr nanoseconds kReportInterval = seconds(60);

    static_assert(kIdCapacity <= UINT8_MAX);

    struct Slot
    {
        char id[kIdCapacity];
        uint8_t size;
        bool wanted;
        bool pending;
    };

    logging::LoggerNode m_log;
    OkxRateLimiter &m_limiter;

    const FileDescriptor *m_epoll;
    network::WebsocketClient m_client;

    std::array<Slot, kInstrumentsCapacity> m_slots;

    nanoseconds m_opened;
    nanoseconds m_received;
    nanoseconds m_pinged;
    nanoseconds m_retry;
    nanoseconds m_backoff;
    nanoseconds m_resume;
    nanoseconds m_reported;

    size_t m_messages;
    size_t m_bytes;
    size_t m_acks;
    size_t m_errors;

    bool connect(nanoseconds now) noexcept;
    void disconnect() noexcept;
    void schedule(nanoseconds now) noexcept;
    void keepalive(nanoseconds now) noexcept;

    void subscribe() noexcept;
    bool submit(bool subscribing) noexcept;
    bool send(size_t size) noexcept;

    void dispatch(std::string_view text) noexcept;
    void report(nanoseconds now) noexcept;

    [[nodiscard]] size_t find(std::string_view id) const noexcept;
    [[nodiscard]] size_t count() const noexcept;
    [[nodiscard]] static std::string_view name(const Slot &slot) noexcept;
};

} // namespace nlx::exchange
#endif // !NLX_EXCHANGE_OKX_CANDLE_STREAMER_H
