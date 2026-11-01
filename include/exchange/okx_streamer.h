#ifndef NLX_EXCHANGE_OKX_STREAMER_H
#define NLX_EXCHANGE_OKX_STREAMER_H

#include "core/config.h"
#include "core/file_descriptor.h"
#include "core/time.h"
#include "exchange/okx_rate_limiter.h"
#include "exchange/okx_types.h"
#include "logging/logger_node.h"
#include "network/websocket_client.h"
#include "parsing/json_reader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace nlx::exchange
{

class OkxStreamer
{
public:
    using CloseHandler = void (*)(void *object, size_t index, const OkxBar &bar) noexcept;

    static constexpr size_t kInstrumentsCapacity = 1024;
    static constexpr size_t kTouchedCapacity = 2 * kInstrumentsCapacity;

    explicit OkxStreamer(const OkxConfig &config, OkxRateLimiter &limiter, logging::LoggerNode &log,
                         const FileDescriptor &epoll, CloseHandler handler, void *object);

    OkxStreamer(const OkxStreamer &) = delete;
    OkxStreamer &operator=(const OkxStreamer &) = delete;
    OkxStreamer(OkxStreamer &&) = delete;
    OkxStreamer &operator=(OkxStreamer &&) = delete;

    void initialize();

    void tick(nanoseconds now) noexcept;
    void receive() noexcept;
    void close() noexcept;

    [[nodiscard]] size_t update(std::span<const OkxInstrument> instruments, std::span<uint16_t, kTouchedCapacity> touched, size_t &ignored) noexcept;

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] std::string_view id(size_t index) const noexcept;

private:
    struct Slot
    {
        char id[OkxInstrument::kIdCapacity];
        uint8_t size;
        bool wanted;
    };

    static constexpr size_t kIndexCapacity = 2 * kInstrumentsCapacity;
    static constexpr size_t kEventCapacity = 1024;
    static constexpr size_t kUrlCapacity = OkxConfig::kCapacity + 32;

    static constexpr milliseconds kConnectTimeout{ 5000 };
    static constexpr milliseconds kReadTimeout{ 5000 };
    static constexpr milliseconds kBarInterval = minutes(1);

    static constexpr nanoseconds kPingInterval = seconds(20);
    static constexpr nanoseconds kPongTimeout = seconds(10);
    static constexpr nanoseconds kReconnectDelay = seconds(1);
    static constexpr nanoseconds kReconnectLimit = seconds(60);
    static constexpr nanoseconds kStableInterval = seconds(60);
    static constexpr nanoseconds kReportInterval = seconds(60);

    static constexpr std::string_view kPath = "/ws/v5/business";

    static_assert(kInstrumentsCapacity < UINT16_MAX);
    static_assert((kIndexCapacity & (kIndexCapacity - 1)) == 0);
    static_assert(OkxInstrument::kIdCapacity <= UINT8_MAX);

    logging::LoggerNode &m_log;
    OkxRateLimiter &m_limiter;
    const FileDescriptor &m_epoll;

    CloseHandler m_handler;
    void *m_object;

    network::WebsocketClient m_client;
    char m_url[kUrlCapacity];

    nanoseconds m_opened;
    nanoseconds m_received;
    nanoseconds m_pinged;
    nanoseconds m_retry;
    nanoseconds m_backoff;
    nanoseconds m_resume;
    nanoseconds m_reported;

    bool m_dirty;

    std::array<Slot, kInstrumentsCapacity> m_slots;
    std::array<uint16_t, kIndexCapacity> m_index;

    size_t m_messages;
    size_t m_updates;
    size_t m_closes;
    size_t m_acks;
    size_t m_errors;
    size_t m_unknown;

    char m_event[kEventCapacity + parsing::json_padding_size];

    bool connect(nanoseconds now) noexcept;
    void disconnect() noexcept;
    void schedule(nanoseconds now) noexcept;
    void keepalive(nanoseconds now) noexcept;

    void subscribe() noexcept;
    bool submit(std::string_view operation, bool wanted) noexcept;
    bool send(size_t size) noexcept;

    void dispatch(std::string_view text) noexcept;
    void record(std::string_view text) noexcept;
    void handle_event(std::string_view text) noexcept;
    void report(nanoseconds now) noexcept;

    void release(size_t index) noexcept;
    void insert(size_t index) noexcept;
    void reindex() noexcept;
    [[nodiscard]] size_t find(std::string_view id) const noexcept;
    [[nodiscard]] static std::string_view name(const Slot &slot) noexcept;
};

} // namespace nlx::exchange
#endif // !NLX_EXCHANGE_OKX_STREAMER_H
