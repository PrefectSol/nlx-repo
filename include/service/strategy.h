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
#include "service/liquid_filter.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
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
    static constexpr milliseconds kCycleInterval{ 60 * 1000 };
    static constexpr nanoseconds kNever{ std::numeric_limits<nanoseconds::rep>::min() / 2 };

    static constexpr size_t kInstrumentsCapacity = 1024;
    static constexpr size_t kLiquidityDaysCapacity = 64;
    static constexpr size_t kBufferCapacity = 2 * 1024 * 1024;

    static constexpr std::string_view kInstrumentType = "SWAP";
    static constexpr std::string_view kSettleCurrency = "USDT";
    static constexpr std::string_view kCryptoCategory = "1";
    static constexpr std::string_view kDailyBar = "1Dutc";

    StrategyConfig m_config;
    logging::LoggerNode m_log;
    exchange::OkxHttpClient &m_okx_http_client;
    LiquidFilter m_liquid_filter;

    FileDescriptor m_epoll;
    FileDescriptor m_stop;
    FileDescriptor m_cycle;
    FileDescriptor m_okx_notify;

    std::atomic<bool> m_initialized;

    size_t m_active_episodes;
    nanoseconds m_instruments_refreshed;

    std::array<exchange::OkxInstrument, kInstrumentsCapacity> m_instruments;
    size_t m_instruments_size;

    std::array<exchange::OkxCandle, kLiquidityDaysCapacity> m_candles;
    std::array<double, kInstrumentsCapacity * kLiquidityDaysCapacity> m_volumes;
    std::array<bool, kInstrumentsCapacity> m_liquid;

    char m_buffer[kBufferCapacity];

    void cycle() noexcept;
    bool refresh_instruments() noexcept;

    bool okx_get_system_time(network::HttpRequest &request, int &code, milliseconds &timestamp) noexcept;
    bool okx_get_account_config(network::HttpRequest &request, int &code, uint64_t &uid, int &mode, bool &hedge, bool &trade) noexcept;
    bool okx_get_positions(network::HttpRequest &request, int &code, size_t &count) noexcept;
    bool okx_get_orders(network::HttpRequest &request, int &code, size_t &count) noexcept;
    bool okx_get_instruments(network::HttpRequest &request, int &code, milliseconds listed_before, std::span<exchange::OkxInstrument> instruments, size_t &count, size_t &ignored) noexcept;
    bool okx_get_daily_candles(network::HttpRequest &request, int &code, std::string_view instrument, milliseconds before, std::span<exchange::OkxCandle> candles, size_t &count) noexcept;
    bool okx_get_balance_usdt(network::HttpRequest &request, int &code, double &available) noexcept;
    bool okx_set_hedge_mode(network::HttpRequest &request, int &code) noexcept;
    bool okx_set_futures_mode(network::HttpRequest &request, int &code) noexcept;
};

} // namespace nlx::service
#endif // !NLX_SERVICE_STRATEGY_H
