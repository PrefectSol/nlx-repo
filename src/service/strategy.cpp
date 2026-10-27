#include "service/strategy.h"
#include "core/fatal.h"
#include "core/time.h"
#include "exchange/okx_types.h"
#include "parsing/json_reader.h"

#include <array>
#include <format>
#include <numeric>
#include <stdexcept>
#include <string_view>

namespace nlx::service
{

Strategy::Strategy(const StrategyConfig &config, exchange::OkxHttpClient &okx_http_client, logging::LoggerRegistry &registry) :
    m_config(config),
    m_log(),
    m_okx_http_client(okx_http_client),
    m_epoll(),
    m_stop(),
    m_update(),
    m_okx_notify(),
    m_initialized(false),
    m_episodes(0),
    m_last_reinstruments(0)
{
    if (m_config.instruments_refresh_minutes <= 0)
    {
        throw std::invalid_argument("invalid strategy configuration");
    }

    m_log = registry.create_node("strategy");

    m_epoll = FileDescriptor::make_epoll();
    m_stop = FileDescriptor::make_eventfd(true);
    m_update = FileDescriptor::make_timerfd(kUpdateInterval, milliseconds::zero(), true);
    m_okx_notify = FileDescriptor::make_eventfd(true);

    detail::epoll_add(m_epoll, m_stop);
    detail::epoll_add(m_epoll, m_update);
}

void Strategy::initialize()
{
    constexpr size_t buffer_capacity = 256 * 1024;
    static_assert(buffer_capacity > parsing::json_padding_size);
    
    char buffer[buffer_capacity];
    network::HttpRequest request{ buffer, sizeof(buffer) - parsing::json_padding_size, 0, 0, false, m_okx_notify };

    int code = -1;

    milliseconds timestamp{};
    bool result = okx_get_system_time(request, code, timestamp);
    if (!result)
    {
        throw std::runtime_error(std::format("okx get time request failed: http = {}, code = {}", request.code, code));
    }

    const std::array<char, time::iso8601_ms_size> servertime = detail::format_iso8601_ms(timestamp);
    m_log.log("okx system time = {}", std::string_view(servertime.data(), servertime.size() - 1));
    m_log.flush();

    uint64_t uid = 0;
    int mode = 0;
    bool hedge = false;
    bool trade = false;
    result = okx_get_account_config(request, code, uid, mode, hedge, trade);
    if (!result)
    {
        throw std::runtime_error(std::format("okx authorization failed: http = {}, code = {}", request.code, code));
    }

    m_log.log("okx uid = {}", uid);
    m_log.log("okx account mode = {}", mode);
    m_log.log("okx hedge mode = {}", hedge);
    m_log.log("okx trade permission = {}", trade);
    m_log.flush();

    size_t positions = 0;
    result = okx_get_positions(request, code, positions);
    if (!result)
    {
        throw std::runtime_error(std::format("okx get positions request failed: http = {}, code = {}", request.code, code));
    }

    m_log.log("okx open positions = {}", positions);
    m_log.flush();

    if (positions != 0)
    {
        throw std::runtime_error(std::format("okx account has {} open positions", positions));
    }

    size_t orders = 0;
    result = okx_get_orders(request, code, orders);
    if (!result)
    {
        throw std::runtime_error(std::format("okx get orders request failed: http = {}, code = {}", request.code, code));
    }

    m_log.log("okx open orders = {}", orders);
    m_log.flush();

    if (orders != 0)
    {
        throw std::runtime_error(std::format("okx account has {} open orders", orders));
    }

    if (mode != 2)
    {
        result = okx_set_futures_mode(request, code);
        if (!result)
        {
            throw std::runtime_error(std::format("okx set account futures mode failed: http = {}, code = {}", request.code, code));
        }

        m_log.log("okx account futures mode enabled");
        m_log.flush();
    }

    if (!hedge)
    {
        result = okx_set_hedge_mode(request, code);
        if (!result)
        {
            throw std::runtime_error(std::format("okx set hedge mode failed: http = {}, code = {}", request.code, code));
        }

        m_log.log("okx account hedge mode enabled");
        m_log.flush();
    }

    m_last_reinstruments = time::now_monotonic() - minutes(m_config.instruments_refresh_minutes) - detail::ceil_minutes(kUpdateInterval);

    m_initialized.store(true, std::memory_order_release);
}

void Strategy::start() noexcept
{
    if (!m_initialized.load(std::memory_order_acquire)) [[unlikely]]
    {
        NLX_FATAL("nlx::service::Strategy::start() called before initialize()");
    }

    m_log.log("strategy started");
    m_log.flush();

    epoll_event event{};
    while (true)
    {
        const size_t count = detail::epoll_wait(m_epoll, { &event, 1 }, -1);
        if (count == 0)
        {
            continue;
        }

        if (event.data.fd == m_update)
        {
            [[maybe_unused]] const uint64_t ticks = detail::drain(m_update);
            update();
        }

        if (event.data.fd == m_stop)
        {
            [[maybe_unused]] const uint64_t result = detail::drain(m_stop);
            break;
        }
    }

    m_log.log("strategy stopped");
    m_log.flush();
}

void Strategy::request_stop() noexcept
{
    detail::notify(m_stop);
}

void Strategy::update() noexcept
{
    const nanoseconds elapsed = time::now_monotonic() - m_last_reinstruments;
    const nanoseconds remaining = minutes(m_config.instruments_refresh_minutes) - elapsed;

    if (m_episodes == 0 && remaining <= nanoseconds::zero())
    {
        const bool refreshed = reinstruments();
        if (refreshed)
        {
            m_last_reinstruments = time::now_monotonic();
        }
    }
}

bool Strategy::reinstruments() noexcept
{
    const nanoseconds begin = time::now_monotonic();

    m_log.log("okx instruments refresh started");
    m_log.flush();

    constexpr size_t buffer_capacity = 1 * 1024 * 1024;
    static_assert(buffer_capacity > parsing::json_padding_size);

    char buffer[buffer_capacity];
    network::HttpRequest request{ buffer, sizeof(buffer) - parsing::json_padding_size, 0, 0, false, m_okx_notify };

    const nanoseconds now = time::now();
    const milliseconds today = detail::floor_days(now);
    const milliseconds since = today - days(kLiquidityDays);

    std::array<exchange::OkxInstrument, kInstrumentsCapacity> instruments{};

    int code = -1;
    size_t listed = 0;
    size_t ignored = 0;

    bool result = okx_get_instruments(request, code, since, instruments, "SWAP", "linear", "USDT", "1", listed, ignored);
    if (!result)
    {
        m_log.log<logging::LogLevel::Error>("okx get instruments request failed: http = {}, code = {}", request.code, code);
        m_log.flush();

        return false;
    }

    if (ignored != 0) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx instruments ignored = {}", ignored);
    }

    if (listed == 0) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx get instruments request found no instruments");
        m_log.flush();

        return false;
    }

    size_t valid = 0;
    size_t liquid = 0;

    std::span<exchange::OkxInstrument> listed_instruments = std::span(instruments).first(listed);
    for (exchange::OkxInstrument &instrument : listed_instruments)
    {
        const std::string_view id(instrument.id);
        std::array<exchange::OkxCandle, kLiquidityDays + 1> candles{};

        size_t count = 0;
        result = okx_get_candles(request, code, id, "1Dutc", detail::ceil_milliseconds(now), std::span(candles), count);
        if (!result)
        {
            if (request.code != 200 || code <= 0)
            {
                m_log.log<logging::LogLevel::Error>("okx {} get candles request failed: http = {}, code = {}", id, request.code, code);
                m_log.flush();

                return false;
            }

            m_log.log<logging::LogLevel::Warning>("okx {} get candles rejected: code = {}", id, code);
            continue;
        }

        const size_t offset = count != 0 && candles[0].time == today ? 1 : 0;
        if (count < offset + kLiquidityDays)
        {
            m_log.log<logging::LogLevel::Warning>("okx {} get candles rejected: days = {}", id, count - offset);
            continue;
        }

        const std::span<const exchange::OkxCandle, kLiquidityDays> closed(candles.data() + offset, kLiquidityDays);

        std::array<double, kLiquidityDays> volumes{};

        bool confirmed = true;
        bool consecutive = true;
        for (size_t index = 0; index < kLiquidityDays; ++index)
        {
            const exchange::OkxCandle &candle = closed[index];

            confirmed = confirmed && candle.confirmed;
            consecutive = consecutive && candle.time == today - days(index + 1);
            volumes[index] = candle.quote_volume;
        }

        if (!confirmed) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Warning>("okx {} get candles are not confirmed yet", id);
            m_log.flush();

            return false;
        }

        if (!consecutive)
        {
            m_log.log<logging::LogLevel::Warning>("okx {} get candles rejected: days are not consecutive", id);
            continue;
        }

        const bool liquid_pass = liquid_filter(volumes);
        instrument.is_liquid = liquid_pass;

        liquid += static_cast<size_t>(liquid_pass);
        ++valid;
    }

    if (valid == 0) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx instruments refresh found no valid instruments: listed = {}", listed);
        m_log.flush();

        return false;
    }

    const nanoseconds end = time::now_monotonic();

    m_log.log("okx instruments refreshed: listed = {}, valid = {}, liquid = {}, elapsed = {} ms", listed, valid, liquid, detail::ms(end - begin));
    m_log.flush();

    return true;
}

bool Strategy::liquid_filter(std::span<const double, kLiquidityDays> volumes) const noexcept
{
    constexpr double threshold = static_cast<double>(kLiquidityThreshold * kLiquidityDays);
    const double sum = std::accumulate(volumes.begin(), volumes.end(), 0.0);

    return sum > threshold;
}

bool Strategy::okx_get_system_time(network::HttpRequest &request, int &code, milliseconds &timestamp) noexcept
{
    code = -1;
    bool result = m_okx_http_client.get_system_time(request);
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_time({ request.buffer, request.size }, code, timestamp);

    return result && request.code == 200 && code == 0;
}

bool Strategy::okx_get_account_config(network::HttpRequest &request, int &code, uint64_t &uid, int &mode, bool &hedge, bool &trade) noexcept
{
    code = -1;
    bool result = m_okx_http_client.get_account_config(request);
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_account_config({ request.buffer, request.size }, code, uid, mode, hedge, trade);

    return result && request.code == 200 && code == 0;
}

bool Strategy::okx_get_positions(network::HttpRequest &request, int &code, size_t &count) noexcept
{
    code = -1;
    bool result = m_okx_http_client.get_positions(request);
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_positions({ request.buffer, request.size }, code, count);

    return result && request.code == 200 && code == 0;
}

bool Strategy::okx_get_orders(network::HttpRequest &request, int &code, size_t &count) noexcept
{
    code = -1;
    bool result = m_okx_http_client.get_pending_orders(request, 100);
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_orders({ request.buffer, request.size }, code, count);

    return result && request.code == 200 && code == 0;
}

bool Strategy::okx_get_instruments(network::HttpRequest &request, int &code, milliseconds listed_before, std::span<exchange::OkxInstrument> instruments,
                                   std::string_view instrument_type, std::string_view contract_type, std::string_view currency, std::string_view category,
                                   size_t &count, size_t &ignored) noexcept
{
    code = -1;
    bool result = m_okx_http_client.get_instruments(request, instrument_type);
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_instruments({ request.buffer, request.size },
                                                    code, currency, category, contract_type, listed_before, instruments, count, ignored);

    return result && request.code == 200 && code == 0;
}

bool Strategy::okx_get_candles(network::HttpRequest &request, int &code, std::string_view instrument, std::string_view bar, milliseconds before, std::span<exchange::OkxCandle> candles, size_t &count) noexcept
{
    code = -1;
    bool result = m_okx_http_client.get_candles(request, instrument, bar, before, static_cast<uint32_t>(candles.size()));
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_candles({ request.buffer, request.size }, code, candles, count);

    return result && request.code == 200 && code == 0;
}

bool Strategy::okx_get_balance(network::HttpRequest &request, int &code, std::string_view currency, double &available) noexcept
{
    code = -1;
    bool result = m_okx_http_client.get_balance(request, currency);
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_balance({ request.buffer, request.size }, code, available, currency);

    return result && request.code == 200 && code == 0;
}

bool Strategy::okx_set_hedge_mode(network::HttpRequest &request, int &code) noexcept
{
    code = -1;
    bool result = m_okx_http_client.set_position_mode(request, true);
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_code({ request.buffer, request.size }, code);

    return result && request.code == 200 && code == 0;
}

bool Strategy::okx_set_futures_mode(network::HttpRequest &request, int &code) noexcept
{
    code = -1;
    bool result = m_okx_http_client.set_account_mode(request, 2);
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_code({ request.buffer, request.size }, code);

    return result && request.code == 200 && code == 0;
}

} // namespace nlx::service
