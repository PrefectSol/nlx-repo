#ifndef NLX_EXCHANGE_OKX_HTTP_CLIENT_H
#define NLX_EXCHANGE_OKX_HTTP_CLIENT_H

#include "core/config.h"
#include "core/credentials.h"
#include "core/crypto.h"
#include "core/file_descriptor.h"
#include "core/time.h"
#include "exchange/okx_rate_limiter.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"
#include "network/http_client.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <string_view>

namespace nlx::exchange
{

class OkxHttpClient
{
public:
    explicit OkxHttpClient(const OkxConfig &config, const Credentials &credentials, OkxRateLimiter &limiter, logging::LoggerRegistry &registry);

    OkxHttpClient(const OkxHttpClient &) = delete;
    OkxHttpClient &operator=(const OkxHttpClient &) = delete;
    OkxHttpClient(OkxHttpClient &&) = delete;
    OkxHttpClient &operator=(OkxHttpClient &&) = delete;

    void initialize();
    void start() noexcept;
    void request_stop() noexcept;

    [[nodiscard]] bool get_system_time(network::HttpRequest &request) noexcept;
    [[nodiscard]] bool get_account_config(network::HttpRequest &request) noexcept;
    [[nodiscard]] bool get_balance(network::HttpRequest &request, std::string_view currency) noexcept;
    [[nodiscard]] bool get_positions(network::HttpRequest &request) noexcept;
    [[nodiscard]] bool get_pending_orders(network::HttpRequest &request, uint32_t limit) noexcept;

    [[nodiscard]] bool set_position_mode(network::HttpRequest &request, bool hedge) noexcept;
    [[nodiscard]] bool set_leverage(network::HttpRequest &request, std::string_view instrument, uint32_t leverage,
                                    std::string_view margin_mode, std::string_view position_side) noexcept;

private:
    static constexpr size_t kSlotsSize = network::HttpClient::kTransfersSize;
    static constexpr size_t kParametersCapacity = 512;
    static constexpr size_t kLineCapacity = 1024;

    static constexpr char kBaseUrl[] = "https://openapi.okx.com";

    static constexpr char kKeyHeader[] = "OK-ACCESS-KEY: ";
    static constexpr char kPassphraseHeader[] = "OK-ACCESS-PASSPHRASE: ";
    static constexpr char kTimestampHeader[] = "OK-ACCESS-TIMESTAMP: ";
    static constexpr char kSignatureHeader[] = "OK-ACCESS-SIGN: ";
    static constexpr char kContentTypeHeader[] = "Content-Type: application/json";
    static constexpr char kSimulatedHeader[] = "x-simulated-trading: 1";

    struct Route
    {
        OkxRequest request;
        network::HttpMethod method;
        std::string_view path;
    };

    static constexpr Route kSystemTimeRoute{ HttpSystemTime, network::HttpMethod::Get, "/api/v5/public/time" };
    static constexpr Route kAccountConfigRoute{ HttpAccountConfig, network::HttpMethod::Get, "/api/v5/account/config" };
    static constexpr Route kBalanceRoute{ HttpBalance, network::HttpMethod::Get, "/api/v5/account/balance" };
    static constexpr Route kPositionsRoute{ HttpPositions, network::HttpMethod::Get, "/api/v5/account/positions" };
    static constexpr Route kPendingOrdersRoute{ HttpPendingOrders, network::HttpMethod::Get, "/api/v5/trade/orders-pending" };
    static constexpr Route kSetPositionModeRoute{ HttpSetPositionMode, network::HttpMethod::Post, "/api/v5/account/set-position-mode" };
    static constexpr Route kSetLeverageRoute{ HttpSetLeverage, network::HttpMethod::Post, "/api/v5/account/set-leverage" };

    enum class SlotState : uint8_t
    {
        Free,
        Queued,
        Running
    };

    struct Slot
    {
        SlotState state;
        bool throttled;
        const Route *route;
        network::HttpRequest *request;
        OkxRateLimiter::Ticket ticket;
        nanoseconds sent;
        size_t size;
        char parameters[kParametersCapacity];
        char timestamp[sizeof(kTimestampHeader) - 1 + time::iso8601_ms_size];
        char signature[sizeof(kSignatureHeader) - 1 + crypto::hmac_sha256_base64_size];
    };

    logging::LoggerNode m_log;
    OkxRateLimiter &m_limiter;

    char m_key[sizeof(kKeyHeader) - 1 + Credentials::kApiFieldCapacity];
    char m_passphrase[sizeof(kPassphraseHeader) - 1 + Credentials::kApiFieldCapacity];
    char m_secret[Credentials::kApiFieldCapacity];
    bool m_simulated;

    std::mutex m_lock;
    std::array<Slot, kSlotsSize> m_slots;
    uint32_t m_rejected;

    FileDescriptor m_stop;
    std::atomic<bool> m_initialized;

    network::HttpClient m_http_client;

    template<typename ...Args>
    bool push(const Route &route, network::HttpRequest &request, std::format_string<Args...> format, Args &&...args) noexcept;

    nanoseconds launch() noexcept;
    void send(size_t index, nanoseconds now) noexcept;
    void complete() noexcept;
    void abort_requests() noexcept;
    void release(Slot &slot) noexcept;
};

} // namespace nlx::exchange
#endif // !NLX_EXCHANGE_OKX_HTTP_CLIENT_H
