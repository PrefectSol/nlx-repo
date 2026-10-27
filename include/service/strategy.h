#ifndef NLX_SERVICE_STRATEGY_H
#define NLX_SERVICE_STRATEGY_H

#include "core/config.h"
#include "core/file_descriptor.h"
#include "core/time.h"
#include "exchange/okx_http_client.h"
#include "exchange/okx_types.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"
#include "network/http_client.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace nlx::service
{

class Strategy
{
public:
    explicit Strategy(const StrategyConfig &config, exchange::OkxHttpClient &okx_http_client, logging::LoggerRegistry &registry);

    Strategy(const Strategy &) = delete;
    Strategy &operator=(const Strategy &) = delete;
    Strategy(Strategy &&) = delete;
    Strategy &operator=(Strategy &&) = delete;

    void initialize();
    void start() noexcept;
    void request_stop() noexcept;

private:
    static constexpr milliseconds kUpdateInterval = milliseconds(60 * 1000);
    static constexpr size_t kInstrumentsCapacity = 1024;
    static constexpr size_t kLiquidityDays = 14;
    static constexpr size_t kLiquidityThreshold = 250'000;

    StrategyConfig m_config;
    logging::LoggerNode m_log;
    exchange::OkxHttpClient &m_okx_http_client;

    FileDescriptor m_epoll;
    FileDescriptor m_stop;
    FileDescriptor m_update;
    FileDescriptor m_okx_notify;

    std::atomic<bool> m_initialized;

    size_t m_episodes;
    nanoseconds m_last_reinstruments;

    void update() noexcept;
    bool reinstruments() noexcept;

    bool liquid_filter(std::span<const double, kLiquidityDays> volumes) const noexcept;

    bool okx_get_system_time(network::HttpRequest &request, int &code, milliseconds &timestamp) noexcept;
    bool okx_get_account_config(network::HttpRequest &request, int &code, uint64_t &uid, int &mode, bool &hedge, bool &trade) noexcept;
    bool okx_get_positions(network::HttpRequest &request, int &code, size_t &count) noexcept;
    bool okx_get_orders(network::HttpRequest &request, int &code, size_t &count) noexcept;
    bool okx_get_instruments(network::HttpRequest &request, int &code, milliseconds listed_before, std::span<exchange::OkxInstrument> instruments, 
                             std::string_view instrument_type, std::string_view contract_type, std::string_view currency, std::string_view category,
                             size_t &count, size_t &ignored) noexcept;
    bool okx_get_candles(network::HttpRequest &request, int &code, std::string_view instrument, std::string_view bar, milliseconds before,
                         std::span<exchange::OkxCandle> candles, size_t &count) noexcept;
    bool okx_get_balance(network::HttpRequest &request, int &code, std::string_view currency, double &available) noexcept;
    bool okx_set_hedge_mode(network::HttpRequest &request, int &code) noexcept;
    bool okx_set_futures_mode(network::HttpRequest &request, int &code) noexcept;
};

} // namespace nlx::service
#endif // !NLX_SERVICE_STRATEGY_H
