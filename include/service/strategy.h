#ifndef NLX_SERVICE_STRATEGY_H
#define NLX_SERVICE_STRATEGY_H

#include "core/file_descriptor.h"
#include "core/market.h"
#include "core/time.h"
#include "exchange/okx_candle_streamer.h"
#include "exchange/okx_http_client.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"
#include "network/http_client.h"
#include "service/screener.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace nlx::service
{

class Strategy
{
public:
    explicit Strategy(exchange::OkxHttpClient &okx_http_client, Screener<exchange::OkxCandleStreamer> &okx_screener, logging::LoggerRegistry &registry);

    Strategy(const Strategy &) = delete;
    Strategy &operator=(const Strategy &) = delete;
    Strategy(Strategy &&) = delete;
    Strategy &operator=(Strategy &&) = delete;

    void initialize();
    void start() noexcept;
    void request_stop() noexcept;

private:
    static constexpr size_t kInstrumentsRefreshMinutes = 1440;

    static constexpr size_t kLiquidityDays = 14;
    static constexpr size_t kLiquidityThreshold = 250'000;
    
    static constexpr size_t kUpdateMilliseconds = 60000;
    static constexpr size_t kInstrumentsCapacity = 1024;

    static constexpr size_t kSignalMinutes = 5;
    static constexpr size_t kSignalThreshold = 5;

    static constexpr size_t kCandlesSlots = network::HttpClient::transfers_size();
    static constexpr size_t kCandlesCapacity = 16 * 1024;

    logging::LoggerNode m_log;
    exchange::OkxHttpClient &m_okx_http_client;
    Screener<exchange::OkxCandleStreamer> &m_okx_screener;

    FileDescriptor m_epoll;
    FileDescriptor m_stop;
    FileDescriptor m_update;
    FileDescriptor m_okx_notify;
    FileDescriptor m_okx_candles_epoll;

    std::array<FileDescriptor, kCandlesSlots> m_okx_candles_notify;

    std::atomic<bool> m_initialized;
    std::atomic<bool> m_stopping;

    size_t m_episodes;
    nanoseconds m_last_reinstruments;

    std::array<int64_t, kSignalThreshold + 1> m_signals;
    size_t m_signals_head;

    void update() noexcept;
    void okx_signals() noexcept;
    bool okx_reinstruments() noexcept;
    bool okx_load_liquidity(std::span<Instrument> instruments, milliseconds today, milliseconds before, size_t &valid, size_t &liquid) noexcept;
    bool okx_check_liquidity(const network::HttpRequest &request, Instrument &instrument, milliseconds today, size_t &valid, size_t &liquid) noexcept;

    bool liquid_filter(std::span<const double, kLiquidityDays> volumes) const noexcept;

    bool okx_get_system_time(network::HttpRequest &request, int &code, milliseconds &timestamp) noexcept;
    bool okx_get_account_config(network::HttpRequest &request, int &code, uint64_t &uid, int &mode, bool &hedge, bool &trade) noexcept;
    bool okx_get_positions(network::HttpRequest &request, int &code, size_t &count) noexcept;
    bool okx_get_orders(network::HttpRequest &request, int &code, size_t &count) noexcept;
    bool okx_get_instruments(network::HttpRequest &request, int &code, milliseconds listed_before, std::span<Instrument> instruments, 
                             std::string_view instrument_type, std::string_view contract_type, std::string_view currency, std::string_view category,
                             size_t &count, size_t &ignored) noexcept;
    bool okx_get_balance(network::HttpRequest &request, int &code, std::string_view currency, double &available) noexcept;
    bool okx_set_hedge_mode(network::HttpRequest &request, int &code) noexcept;
    bool okx_set_futures_mode(network::HttpRequest &request, int &code) noexcept;
};

} // namespace nlx::service
#endif // !NLX_SERVICE_STRATEGY_H
