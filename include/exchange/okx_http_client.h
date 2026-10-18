#ifndef NLX_EXCHANGE_OKX_HTTP_CLIENT_H
#define NLX_EXCHANGE_OKX_HTTP_CLIENT_H

#include "core/config.h"
#include "core/credentials.h"
#include "core/crypto.h"
#include "core/time.h"
#include "exchange/okx_rate_limiter.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"
#include "network/http_client.h"

#include <curl/curl.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string_view>

namespace nlx::exchange
{

class OkxHttpClient
{
public:
    explicit OkxHttpClient(const OkxConfig &config, const Credentials &credentials,
                           OkxRateLimiter &limiter, logging::LoggerRegistry &registry);
    ~OkxHttpClient() noexcept;

    OkxHttpClient(const OkxHttpClient &) = delete;
    OkxHttpClient &operator=(const OkxHttpClient &) = delete;
    OkxHttpClient(OkxHttpClient &&) = delete;
    OkxHttpClient &operator=(OkxHttpClient &&) = delete;

    void start() noexcept;
    void request_stop() noexcept;

    [[nodiscard]] bool get_time(network::HttpRequest &request) noexcept;
    [[nodiscard]] bool get_account_config(network::HttpRequest &request) noexcept;
    [[nodiscard]] bool get_balance(network::HttpRequest &request, std::string_view currency) noexcept;
    [[nodiscard]] bool get_positions(network::HttpRequest &request) noexcept;
    [[nodiscard]] bool get_pending_orders(network::HttpRequest &request, uint32_t limit) noexcept;

    [[nodiscard]] bool set_position_mode(network::HttpRequest &request, bool hedge) noexcept;
    [[nodiscard]] bool set_leverage(network::HttpRequest &request, std::string_view instrument, uint32_t leverage,
                                    std::string_view margin_mode, std::string_view position_side) noexcept;

private:
    struct Route
    {
        OkxRequest request;
        std::string_view path;
        bool sign;
    };

    static constexpr char kBaseUrl[] = "https://openapi.okx.com";

    static constexpr Route kTimeRoute{ HttpTime, "/api/v5/public/time", false };
    static constexpr Route kAccountConfigRoute{ HttpAccountConfig, "/api/v5/account/config", true };
    static constexpr Route kBalanceRoute{ HttpBalance, "/api/v5/account/balance", true };
    static constexpr Route kPositionsRoute{ HttpPositions, "/api/v5/account/positions", true };
    static constexpr Route kPendingOrdersRoute{ HttpPendingOrders, "/api/v5/trade/orders-pending", true };
    static constexpr Route kSetPositionModeRoute{ HttpSetPositionMode, "/api/v5/account/set-position-mode", true };
    static constexpr Route kSetLeverageRoute{ HttpSetLeverage, "/api/v5/account/set-leverage", true };

    static constexpr size_t kTargetCapacity = 128;
    static constexpr size_t kBodyCapacity = 128;

    static constexpr size_t kSlotsSize = sizeof(uint8_t) * 8;
    static constexpr uint8_t kSlotsMask = std::numeric_limits<uint8_t>::max();

    static_assert(kSlotsSize == std::numeric_limits<uint8_t>::digits);

    static constexpr char kKeyHeader[] = "OK-ACCESS-KEY: ";
    static constexpr char kPassphraseHeader[] = "OK-ACCESS-PASSPHRASE: ";
    static constexpr char kTimestampHeader[] = "OK-ACCESS-TIMESTAMP: ";
    static constexpr char kSignHeader[] = "OK-ACCESS-SIGN: ";
    static constexpr char kContentTypeHeader[] = "Content-Type: application/json";
    static constexpr char kSimulatedHeader[] = "x-simulated-trading: 1";

    struct Slot
    {
        CURL *handle;
        const Route *route;
        network::HttpRequest *request;
        OkxRateLimiter::Token token;
        nanoseconds sent;
        size_t body_size;
        char url[sizeof(kBaseUrl) - 1 + kTargetCapacity];
        char body[kBodyCapacity];
        char timestamp[sizeof(kTimestampHeader) - 1 + time::iso8601_ms_size];
        char sign[sizeof(kSignHeader) - 1 + crypto::hmac_sha256_base64_size];
        curl_slist headers[5];
    };

    const Credentials &m_credentials;
    OkxRateLimiter &m_limiter;
    logging::LoggerNode m_log;

    std::atomic<bool> m_running;
    std::mutex m_lock;

    std::atomic<uint8_t> m_busy;
    uint8_t m_launched;
    uint8_t m_throttled;

    char m_key[sizeof(kKeyHeader) - 1 + Credentials::kApiFieldCapacity];
    char m_passphrase[sizeof(kPassphraseHeader) - 1 + Credentials::kApiFieldCapacity];

    curl_slist m_simulated[1];
    curl_slist *m_public_headers;

    network::HttpClient m_http_client;
    std::array<Slot, kSlotsSize> m_slots;

    bool push(const Route &route, network::HttpRequest &request, std::string_view query, std::string_view body) noexcept;
    nanoseconds launch() noexcept;
    void complete() noexcept;
    void abort() noexcept;
    void release(size_t index, network::HttpRequest &request) noexcept;
    void sign(Slot &slot) noexcept;
};

} // namespace nlx::exchange
#endif // !NLX_EXCHANGE_OKX_HTTP_CLIENT_H
