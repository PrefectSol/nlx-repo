#ifndef NLX_EXCHANGE_OKX_WEBSOCKET_H
#define NLX_EXCHANGE_OKX_WEBSOCKET_H

#include "core/config.h"
#include "core/credentials.h"
#include "core/file_descriptor.h"
#include "core/time.h"
#include "exchange/okx_rate_limiter.h"
#include "exchange/okx_types.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"
#include "network/websocket_client.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <span>
#include <string_view>

namespace nlx::exchange
{

class OkxWebsocket
{
public:
    explicit OkxWebsocket(const OkxConfig &config, const Credentials &credentials, OkxRateLimiter &limiter, logging::LoggerRegistry &registry);

    OkxWebsocket(const OkxWebsocket &) = delete;
    OkxWebsocket &operator=(const OkxWebsocket &) = delete;
    OkxWebsocket(OkxWebsocket &&) = delete;
    OkxWebsocket &operator=(OkxWebsocket &&) = delete;

    void initialize();
    void start() noexcept;
    void request_stop() noexcept;

    [[nodiscard]] bool resubscribe(std::span<const OkxInstrument> instruments) noexcept;

    static constexpr size_t kInstrumentsCapacity = 1024;

private:
    static constexpr size_t kSubscriptionsCapacity = 2 * kInstrumentsCapacity;
    static constexpr size_t kRequestCapacity = 32 * 1024;
    static constexpr size_t kUrlCapacity = OkxConfig::kCapacity + 32;

    static constexpr milliseconds kTickInterval{ 1000 };
    static constexpr milliseconds kConnectTimeout{ 5000 };
    static constexpr milliseconds kReadTimeout{ 5000 };

    static constexpr nanoseconds kPingInterval = seconds(20);
    static constexpr nanoseconds kPongTimeout = seconds(10);
    static constexpr nanoseconds kReconnectDelay = seconds(1);
    static constexpr nanoseconds kReconnectLimit = seconds(60);
    static constexpr nanoseconds kStableInterval = seconds(60);

    static constexpr std::string_view kBusinessPath = "/ws/v5/business";
    static constexpr std::string_view kPrivatePath = "/ws/v5/private";
    static constexpr std::string_view kCandleChannel = "candle1m";

    struct Connection
    {
        std::string_view name;
        OkxRequest request;
        char url[kUrlCapacity];

        network::WebsocketClient client;

        nanoseconds opened;
        nanoseconds received;
        nanoseconds pinged;
        nanoseconds retry;
        nanoseconds backoff;
    };

    struct Subscription
    {
        char id[OkxInstrument::kIdCapacity];
        bool wanted;
        bool active;
    };

    logging::LoggerNode m_log;
    OkxRateLimiter &m_limiter;

    FileDescriptor m_epoll;
    FileDescriptor m_stop;
    FileDescriptor m_update;
    FileDescriptor m_timer;

    std::atomic<bool> m_initialized;

    char m_key[Credentials::kApiFieldCapacity];
    char m_passphrase[Credentials::kApiFieldCapacity];
    char m_secret[Credentials::kApiFieldCapacity];

    Connection m_business;
    Connection m_private;

    std::array<OkxInstrument, kInstrumentsCapacity> m_pending;
    size_t m_pending_size;
    std::atomic<bool> m_pending_ready;

    std::array<Subscription, kSubscriptionsCapacity> m_subscriptions;
    size_t m_subscriptions_size;
    bool m_dirty;
    nanoseconds m_resume;

    char m_request[kRequestCapacity];

    void tick() noexcept;
    void update() noexcept;
    void reset() noexcept;
    void reconcile() noexcept;
    bool apply(std::string_view operation, bool wanted) noexcept;

    bool connect(Connection &connection) noexcept;
    void disconnect(Connection &connection) noexcept;
    void close(Connection &connection) noexcept;
    void schedule(Connection &connection, nanoseconds now) noexcept;

    void login() noexcept;
    void keepalive(Connection &connection) noexcept;
    void receive(Connection &connection) noexcept;
    bool send(Connection &connection, std::string_view text) noexcept;
};

} // namespace nlx::exchange
#endif // !NLX_EXCHANGE_OKX_WEBSOCKET_H
