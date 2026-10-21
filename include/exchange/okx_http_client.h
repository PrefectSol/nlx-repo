#ifndef NLX_EXCHANGE_OKX_HTTP_CLIENT_H
#define NLX_EXCHANGE_OKX_HTTP_CLIENT_H

#include "core/config.h"
#include "core/credentials.h"
#include "core/file_descriptor.h"
#include "core/time.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"
#include "exchange/okx_rate_limiter.h"
#include "network/http_client.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
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
    [[nodiscard]] bool set_account_mode(network::HttpRequest &request, uint32_t level) noexcept;

private:
    using SlotMask = network::HttpClient::SlotMask;

    static constexpr size_t kParametersCapacity = 256;
    static constexpr size_t kHeaderCapacity = 64;

    static constexpr std::string_view kBaseUrl = "https://openapi.okx.com";

    struct Route
    {
        OkxRequest request;
        bool post;
        std::string_view path;
    };

    struct Slot
    {
        const Route *route;
        network::HttpRequest *request;

        char parameters[kParametersCapacity];
        size_t size;

        char timestamp[kHeaderCapacity];
        char signature[kHeaderCapacity];
    };

    static constexpr Route kSystemTimeRoute{ HttpSystemTime, false, "/api/v5/public/time" };
    static constexpr Route kAccountConfigRoute{ HttpAccountConfig, false, "/api/v5/account/config" };
    static constexpr Route kBalanceRoute{ HttpBalance, false, "/api/v5/account/balance" };
    static constexpr Route kPositionsRoute{ HttpPositions, false, "/api/v5/account/positions" };
    static constexpr Route kPendingOrdersRoute{ HttpPendingOrders, false, "/api/v5/trade/orders-pending" };
    static constexpr Route kSetPositionModeRoute{ HttpSetPositionMode, true, "/api/v5/account/set-position-mode" };
    static constexpr Route kSetAccountModeRoute{ HttpSetAccountMode, true, "/api/v5/account/set-account-level" };

    logging::LoggerNode m_log;
    OkxRateLimiter &m_limiter;

    FileDescriptor m_stop;
    std::atomic<bool> m_initialized;
    std::atomic<bool> m_stopped;

    network::HttpClient m_http_client;

    char m_key[kHeaderCapacity];
    char m_passphrase[kHeaderCapacity];
    char m_simulated[kHeaderCapacity];
    char m_secret[Credentials::kApiFieldCapacity];

    std::array<Slot, network::HttpClient::kTransfersSize> m_slots;

    std::atomic<SlotMask> m_busy;
    std::atomic<SlotMask> m_ready;

    SlotMask m_launched;
    SlotMask m_frozen;
    nanoseconds m_thaw;

    template<typename ...Args>
    bool push(const Route &route, network::HttpRequest &request, std::format_string<Args...> format, Args &&...args) noexcept;

    bool claim(size_t &index) noexcept;
    void vacate(size_t index) noexcept;

    milliseconds schedule() noexcept;
    void dispatch(size_t index) noexcept;
    void complete() noexcept;
    void release() noexcept;
};

} // namespace nlx::exchange
#endif // !NLX_EXCHANGE_OKX_HTTP_CLIENT_H
