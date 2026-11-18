#ifndef NLX_SERVICE_STRATEGY_H
#define NLX_SERVICE_STRATEGY_H

#include "core/file_descriptor.h"
#include "core/market.h"
#include "core/ring_buffer.h"
#include "core/time.h"
#include "exchange/okx_account_streamer.h"
#include "exchange/okx_candle_streamer.h"
#include "exchange/okx_http_client.h"
#include "exchange/okx_market_streamer.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"
#include "network/http_client.h"
#include "service/account.h"
#include "service/market.h"
#include "service/minimum_order.h"
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
    explicit Strategy(exchange::OkxHttpClient &okx_http_client, Screener<exchange::OkxCandleStreamer> &okx_screener,
                      Market<exchange::OkxMarketStreamer> &okx_market, Account<exchange::OkxAccountStreamer> &okx_account,
                      logging::LoggerRegistry &registry);

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
    static constexpr size_t kTickMilliseconds = 1000;
    static constexpr size_t kInstrumentsCapacity = 1024;

    static constexpr size_t kCascadeMinutes = 5;
    static constexpr size_t kCascadeSignals = 5;

    static constexpr size_t kEpisodesCapacity = 3;
    static constexpr size_t kEpisodeMinutes = 7;
    static constexpr size_t kWarmupMinutes = 2;
    static constexpr size_t kHistoryBars = 90;
    static constexpr size_t kLeverage = 5;

    static constexpr size_t kBalanceMilliseconds = 15000;
    static constexpr size_t kEpisodeRequestCapacity = 64 * 1024;

    static constexpr size_t kFeesCapacity = 16;
    static constexpr size_t kTiersCapacity = 256;

    static constexpr size_t kCandlesSlots = network::HttpClient::transfers_size();
    static constexpr size_t kCandlesCapacity = 16 * 1024;

    static_assert(kEpisodesCapacity <= Market<exchange::OkxMarketStreamer>::capacity());
    static_assert(kWarmupMinutes * 60 >= kHistoryBars);

    enum class Stage : uint8_t
    {
        Idle,
        Tiers,
        LeverageLong,
        LeverageShort,
        Warmup,
        Ready
    };

    struct Episode
    {
        uint64_t id;
        size_t instrument;
        nanoseconds opened;
        double budget;
        double price;
        double fee;
        double leverage;
        Stage stage;
        bool pending;
    };

    logging::LoggerNode m_log;
    exchange::OkxHttpClient &m_okx_http_client;
    Screener<exchange::OkxCandleStreamer> &m_okx_screener;
    Market<exchange::OkxMarketStreamer> &m_okx_market;
    Account<exchange::OkxAccountStreamer> &m_okx_account;

    FileDescriptor m_epoll;
    FileDescriptor m_stop;
    FileDescriptor m_update;
    FileDescriptor m_tick;
    FileDescriptor m_okx_notify;
    FileDescriptor m_okx_candles_epoll;

    std::array<FileDescriptor, kCandlesSlots> m_okx_candles_notify;
    std::array<FileDescriptor, kEpisodesCapacity> m_okx_episodes_notify;

    std::atomic<bool> m_initialized;
    std::atomic<bool> m_stopping;

    nanoseconds m_last_reinstruments;

    std::array<Instrument, kInstrumentsCapacity> m_instruments;
    size_t m_instruments_size;

    std::array<Fee, kFeesCapacity> m_fees;
    size_t m_fees_size;

    Balance m_balance;
    nanoseconds m_balance_at;

    std::array<Episode, kEpisodesCapacity> m_episodes;
    std::array<RingBuffer<Bar, kHistoryBars>, kEpisodesCapacity> m_histories;
    std::array<std::optional<network::HttpRequest>, kEpisodesCapacity> m_requests;
    char m_buffers[kEpisodesCapacity][kEpisodeRequestCapacity];

    uint64_t m_opened;
    size_t m_active;

    std::array<int64_t, kCascadeSignals + 1> m_signals;
    size_t m_signals_head;

    void update() noexcept;
    void tick() noexcept;

    void okx_signals() noexcept;
    void okx_market() noexcept;
    void okx_account() noexcept;

    void okx_open_episode(const ScreenerSignal &signal) noexcept;
    void okx_close_episode(size_t slot, std::string_view reason) noexcept;
    void okx_episode_response(size_t slot) noexcept;
    void okx_episode_tiers(size_t slot, const network::HttpRequest &request) noexcept;
    void okx_episode_leverage(size_t slot, const network::HttpRequest &request) noexcept;
    void okx_episode_bar(size_t slot, const Bar &bar) noexcept;
    bool okx_episode_request(size_t slot, Stage stage) noexcept;

    size_t find_instrument(std::string_view id) const noexcept;
    const Fee *find_fee(int group) const noexcept;
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
    bool okx_get_balance(network::HttpRequest &request, int &code, std::string_view currency, Balance &balance) noexcept;
    bool okx_get_fees(network::HttpRequest &request, int &code, std::span<Fee> fees, size_t &count) noexcept;
    bool okx_set_hedge_mode(network::HttpRequest &request, int &code) noexcept;
    bool okx_set_futures_mode(network::HttpRequest &request, int &code) noexcept;
};

} // namespace nlx::service
#endif // !NLX_SERVICE_STRATEGY_H
