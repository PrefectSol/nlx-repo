#ifndef NLX_EXCHANGE_OKX_ACCOUNT_STREAMER_H
#define NLX_EXCHANGE_OKX_ACCOUNT_STREAMER_H

#include "core/credentials.h"
#include "core/file_descriptor.h"
#include "core/market.h"
#include "core/time.h"
#include "exchange/okx_rate_limiter.h"
#include "logging/logger_node.h"
#include "network/websocket_client.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace nlx::exchange
{

class OkxAccountStreamer
{
public:
    explicit OkxAccountStreamer(const Credentials &credentials, OkxRateLimiter &limiter, logging::LoggerNode &log);

    OkxAccountStreamer(const OkxAccountStreamer &) = delete;
    OkxAccountStreamer &operator=(const OkxAccountStreamer &) = delete;
    OkxAccountStreamer(OkxAccountStreamer &&) = delete;
    OkxAccountStreamer &operator=(OkxAccountStreamer &&) = delete;

    void initialize();

    [[nodiscard]] bool poll(AccountUpdateType &type) noexcept;

    void heartbeat() noexcept;
    void close() noexcept;

    [[nodiscard]] const FileDescriptor &fd() const noexcept;
    [[nodiscard]] bool connected() const noexcept;

    [[nodiscard]] const Balance &balance() const noexcept;
    [[nodiscard]] int64_t timestamp() const noexcept;
    [[nodiscard]] std::span<const Position> positions() const noexcept;
    [[nodiscard]] std::span<const Order> orders() const noexcept;

private:
    static constexpr char kUrl[] = "wss://ws.okx.com/ws/v5/private";
    static constexpr char kCurrency[] = "USDT";

    static constexpr size_t kPositionsCapacity = 64;
    static constexpr size_t kOrdersCapacity = 64;
    static constexpr size_t kConnectMilliseconds = 5000;
    static constexpr size_t kLoginMilliseconds = 3000;
    static constexpr size_t kReadMilliseconds = 1000;
    static constexpr size_t kHeartbeatMilliseconds = 15000;
    static constexpr size_t kBackoffMilliseconds = 1000;
    static constexpr size_t kBackoffLimitMilliseconds = 30000;

    logging::LoggerNode &m_log;
    OkxRateLimiter &m_limiter;

    FileDescriptor m_epoll;
    network::WebsocketClient m_client;

    char m_key[Credentials::kApiFieldCapacity];
    char m_passphrase[Credentials::kApiFieldCapacity];
    char m_secret[Credentials::kApiFieldCapacity];

    Balance m_balance;
    int64_t m_timestamp;

    std::array<Position, kPositionsCapacity> m_positions;
    size_t m_positions_size;

    std::array<Order, kOrdersCapacity> m_orders;
    size_t m_orders_size;

    nanoseconds m_received;
    nanoseconds m_pinged_at;
    bool m_pinged;

    nanoseconds m_retry_at;
    milliseconds m_backoff;
    bool m_upgrade;

    bool reconnect() noexcept;
    bool connect() noexcept;
    bool login() noexcept;
    bool subscribe() noexcept;
    void acquire(OkxRequest request) noexcept;

    bool send(size_t size) noexcept;
    bool read(std::string_view &text) noexcept;
    bool extract(std::string_view text, AccountUpdateType &type) noexcept;
    void dispatch(std::string_view text) noexcept;
};

} // namespace nlx::exchange
#endif // !NLX_EXCHANGE_OKX_ACCOUNT_STREAMER_H
