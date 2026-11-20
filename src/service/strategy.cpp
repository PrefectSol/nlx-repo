#include "service/strategy.h"
#include "core/fatal.h"
#include "core/market.h"
#include "core/time.h"
#include "parsing/json_reader.h"
#include "service/minimum_order.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace nlx::service
{

namespace
{

constexpr std::string_view stage_name(uint8_t stage) noexcept
{
    constexpr std::array<std::string_view, 6> names = { "idle", "tiers", "leverage long", "leverage short", "warmup", "ready" };
    return stage < names.size() ? names[stage] : "unknown";
}

std::string_view okx_message(const network::HttpRequest &request, int code) noexcept
{
    return code > 0 ? parsing::detail::okx_parse_message({ request.buffer, request.size }) : std::string_view();
}

} // namespace

Strategy::Strategy(exchange::OkxHttpClient &okx_http_client, Screener<exchange::OkxCandleStreamer> &okx_screener,
                   Market<exchange::OkxMarketStreamer> &okx_market, Account<exchange::OkxAccountStreamer> &okx_account,
                   Model &model, logging::LoggerRegistry &registry) :
    m_log(),
    m_okx_http_client(okx_http_client),
    m_okx_screener(okx_screener),
    m_okx_market(okx_market),
    m_okx_account(okx_account),
    m_model(model),
    m_epoll(),
    m_stop(),
    m_update(),
    m_tick(),
    m_okx_notify(),
    m_okx_candles_epoll(),
    m_okx_candles_notify(),
    m_okx_episodes_notify(),
    m_model_notify(),
    m_initialized(false),
    m_stopping(false),
    m_last_reinstruments(0),
    m_instruments(),
    m_instruments_size(0),
    m_fees(),
    m_fees_size(0),
    m_balance(),
    m_balance_at(0),
    m_episodes(),
    m_histories(),
    m_requests(),
    m_buffers(),
    m_backlogs(),
    m_inputs(),
    m_states(),
    m_outputs(),
    m_loop(),
    m_bar_delay(),
    m_model_trip(),
    m_decision_delay(),
    m_ticks(0),
    m_opened(0),
    m_active(0),
    m_signals(),
    m_signals_head(0)
{
    m_log = registry.create_node("strategy");

    m_epoll = FileDescriptor::make_epoll();
    m_stop = FileDescriptor::make_eventfd(true);
    m_update = FileDescriptor::make_timerfd(milliseconds(kUpdateMilliseconds), milliseconds::zero(), true);
    m_tick = FileDescriptor::make_timerfd(milliseconds(kTickMilliseconds), true);
    m_okx_notify = FileDescriptor::make_eventfd(true);
    m_okx_candles_epoll = FileDescriptor::make_epoll();

    for (FileDescriptor &notify : m_okx_candles_notify)
    {
        notify = FileDescriptor::make_eventfd(true);
        detail::epoll_add(m_okx_candles_epoll, notify);
    }

    for (FileDescriptor &notify : m_okx_episodes_notify)
    {
        notify = FileDescriptor::make_eventfd(true);
        detail::epoll_add(m_epoll, notify);
    }

    for (FileDescriptor &notify : m_model_notify)
    {
        notify = FileDescriptor::make_eventfd(true);
        detail::epoll_add(m_epoll, notify);
    }

    detail::epoll_add(m_epoll, m_stop);
    detail::epoll_add(m_epoll, m_update);
    detail::epoll_add(m_epoll, m_tick);
    detail::epoll_add(m_epoll, m_okx_screener.signals());
    detail::epoll_add(m_epoll, m_okx_market.events());
    detail::epoll_add(m_epoll, m_okx_account.events());
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
        throw std::runtime_error(std::format("okx get time request failed: http = {}, code = {}, message = {}", request.code, code, okx_message(request, code)));
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
        throw std::runtime_error(std::format("okx authorization failed: http = {}, code = {}, message = {}", request.code, code, okx_message(request, code)));
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
        throw std::runtime_error(std::format("okx get positions request failed: http = {}, code = {}, message = {}", request.code, code, okx_message(request, code)));
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
        throw std::runtime_error(std::format("okx get orders request failed: http = {}, code = {}, message = {}", request.code, code, okx_message(request, code)));
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
            throw std::runtime_error(std::format("okx set account futures mode failed: http = {}, code = {}, message = {}", request.code, code, okx_message(request, code)));
        }

        m_log.log("okx account futures mode enabled");
        m_log.flush();
    }

    if (!hedge)
    {
        result = okx_set_hedge_mode(request, code);
        if (!result)
        {
            throw std::runtime_error(std::format("okx set hedge mode failed: http = {}, code = {}, message = {}", request.code, code, okx_message(request, code)));
        }

        m_log.log("okx account hedge mode enabled");
        m_log.flush();
    }

    result = okx_get_fees(request, code, m_fees, m_fees_size);
    if (!result || m_fees_size == 0)
    {
        throw std::runtime_error(std::format("okx get trade fee request failed: http = {}, code = {}, message = {}", request.code, code, okx_message(request, code)));
    }

    for (size_t index = 0; index < m_fees_size; ++index)
    {
        const Fee &fee = m_fees[index];
        m_log.log("okx swap fee group {}: maker = {}, taker = {}", fee.group, fee.maker, fee.taker);
    }

    m_log.flush();

    Balance balance{};
    result = okx_get_balance(request, code, "USDT", balance);
    if (!result)
    {
        throw std::runtime_error(std::format("okx get balance request failed: http = {}, code = {}, message = {}", request.code, code, okx_message(request, code)));
    }

    m_log.log("okx balance: equity = {}, available = {}, isolated pnl = {}", balance.equity, balance.available, balance.isolated_pnl);
    m_log.flush();

    m_balance = balance;
    m_balance_at = time::now_monotonic();

    if (balance.equity <= 0.0)
    {
        m_log.log<logging::LogLevel::Warning>("okx balance is empty: episodes will be rejected");
        m_log.flush();
    }

    const bool fits = m_model.input_size() <= kModelInputCapacity && m_model.state_size() <= kModelStateCapacity && m_model.output_size() <= kModelOutputCapacity;
    if (m_model.ready() && !fits)
    {
        throw std::runtime_error(std::format("model does not fit strategy buffers: input = {}, state = {}, output = {}",
                                             m_model.input_size(), m_model.state_size(), m_model.output_size()));
    }

    m_last_reinstruments = time::now_monotonic() - minutes(kInstrumentsRefreshMinutes) - detail::ceil_minutes(milliseconds(kUpdateMilliseconds));

    m_initialized.store(true, std::memory_order_release);
}

void Strategy::start() noexcept
{
    if (!m_initialized.load(std::memory_order_acquire)) [[unlikely]]
    {
        NLX_FATAL("nlx::service::Strategy::start() called before initialize()");
    }

    m_log.log("strategy started");
    m_log.log("instruments refresh minutes: {}", kInstrumentsRefreshMinutes);
    m_log.log("instruments liquidity days: {}", kLiquidityDays);
    m_log.log("instruments liquidity threshold: {}", kLiquidityThreshold);
    m_log.log("cascade: signals = {}, minutes = {}", kCascadeSignals, kCascadeMinutes);
    m_log.log("episodes: capacity = {}, minutes = {}, warmup minutes = {}, history bars = {}, leverage = {}",
              kEpisodesCapacity, kEpisodeMinutes, kWarmupMinutes, kHistoryBars, kLeverage);
    m_log.log("decisions: model = {}, hold seconds = {}, orders = market", m_model.ready(), kDecisionSeconds);
    m_log.flush();

    m_loop.reset(time::now_monotonic());

    epoll_event event{};
    while (true)
    {
        m_loop.sleep(time::now_monotonic());
        const size_t count = detail::epoll_wait(m_epoll, { &event, 1 }, -1);
        m_loop.wake(time::now_monotonic());

        if (m_stopping.load(std::memory_order_acquire))
        {
            break;
        }

        if (count == 0)
        {
            continue;
        }

        if (event.data.fd == m_okx_market.events())
        {
            [[maybe_unused]] const uint64_t result = detail::drain(m_okx_market.events());
            okx_market();
        }

        for (size_t slot = 0; slot < kEpisodesCapacity; ++slot)
        {
            if (event.data.fd == m_model_notify[slot])
            {
                [[maybe_unused]] const uint64_t result = detail::drain(m_model_notify[slot]);
                okx_inferred(slot);
            }
        }

        for (size_t slot = 0; slot < kEpisodesCapacity; ++slot)
        {
            if (event.data.fd == m_okx_episodes_notify[slot])
            {
                [[maybe_unused]] const uint64_t result = detail::drain(m_okx_episodes_notify[slot]);
                okx_episode_response(slot);
            }
        }

        if (event.data.fd == m_okx_account.events())
        {
            [[maybe_unused]] const uint64_t result = detail::drain(m_okx_account.events());
            okx_account();
        }

        if (event.data.fd == m_okx_screener.signals())
        {
            [[maybe_unused]] const uint64_t result = detail::drain(m_okx_screener.signals());
            okx_signals();
        }

        if (event.data.fd == m_tick)
        {
            [[maybe_unused]] const uint64_t ticks = detail::drain(m_tick);
            tick();
        }

        if (event.data.fd == m_update)
        {
            [[maybe_unused]] const uint64_t ticks = detail::drain(m_update);
            update();
        }
    }

    m_log.log("strategy stopped");
    m_log.flush();
}

void Strategy::request_stop() noexcept
{
    m_stopping.store(true, std::memory_order_release);
    detail::notify(m_stop);
}

void Strategy::update() noexcept
{
    const nanoseconds elapsed = time::now_monotonic() - m_last_reinstruments;
    const nanoseconds remaining = minutes(kInstrumentsRefreshMinutes) - elapsed;

    if (m_active == 0 && remaining <= nanoseconds::zero())
    {
        const bool refreshed = okx_reinstruments();
        if (refreshed)
        {
            m_last_reinstruments = time::now_monotonic();
        }

        const size_t discarded = m_okx_screener.discard();
        if (discarded != 0)
        {
            m_log.log<logging::LogLevel::Warning>("okx signals discarded during instruments refresh: {}", discarded);
            m_log.flush();
        }
    }
}

void Strategy::tick() noexcept
{
    const nanoseconds now = time::now_monotonic();

    bool closed = false;
    for (size_t slot = 0; slot < kEpisodesCapacity; ++slot)
    {
        Episode &episode = m_episodes[slot];

        const bool expired = episode.stage != Stage::Idle && now - episode.opened >= minutes(kEpisodeMinutes);
        if (expired)
        {
            okx_close_episode(slot, "expired");
            closed = true;
        }

        const bool leveraging = episode.stage == Stage::LeverageLong || episode.stage == Stage::LeverageShort;
        const bool due = leveraging && !episode.pending && episode.retry != nanoseconds::zero() && now >= episode.retry;
        if (!due)
        {
            continue;
        }

        episode.retry = nanoseconds::zero();
        if (!okx_episode_request(slot, episode.stage)) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Error>("okx {} set leverage retry rejected by http client", std::string_view(m_instruments[episode.instrument].id));
            okx_close_episode(slot, "leverage");
            closed = true;
        }
    }

    if (closed)
    {
        m_log.flush();
    }

    ++m_ticks;
    if (m_ticks % kMetricsTicks == 0)
    {
        report();
    }
}

void Strategy::report() noexcept
{
    const nanoseconds now = time::now_monotonic();

    m_log.log<logging::LogLevel::Metric>("strategy metrics: utilization = {} %, wakeups = {}, active = {}, bars = {}, bar delay p50 = {} ms, p99 = {} ms",
              m_loop.utilization(now), m_loop.wakeups(), m_active, m_bar_delay.count(), m_bar_delay.percentile(0.5) / 1000.0, m_bar_delay.percentile(0.99) / 1000.0);
    m_log.log<logging::LogLevel::Metric>("strategy metrics: model trips = {}, trip p50 = {} us, p99 = {} us, decisions = {}, decision delay p50 = {} ms, p99 = {} ms",
              m_model_trip.count(), m_model_trip.percentile(0.5), m_model_trip.percentile(0.99), m_decision_delay.count(),
              m_decision_delay.percentile(0.5) / 1000.0, m_decision_delay.percentile(0.99) / 1000.0);
    m_log.flush();

    m_loop.reset(now);
    m_bar_delay.reset();
    m_model_trip.reset();
    m_decision_delay.reset();
}

void Strategy::okx_signals() noexcept
{
    constexpr int64_t window = static_cast<int64_t>(kCascadeMinutes) * 60000;

    ScreenerSignal signal{};
    while (m_okx_screener.pop(signal))
    {
        const std::string_view instrument(signal.instrument);

        m_signals[m_signals_head % m_signals.size()] = signal.timestamp;
        ++m_signals_head;

        const size_t stored = std::min(m_signals_head, m_signals.size());
        const int64_t since = signal.timestamp - window;

        size_t count = 0;
        for (size_t index = 0; index < stored; ++index)
        {
            count += static_cast<size_t>(m_signals[index] > since);
        }

        m_log.log("okx signal: instrument = {}, timestamp = {}, close = {}, impulse = {}, baseline = {}, signals = {}",
                  instrument, signal.timestamp, signal.close, signal.impulse, signal.baseline, count);

        if (count > kCascadeSignals)
        {
            m_okx_screener.cooldown();
            m_signals_head = 0;

            m_log.log<logging::LogLevel::Warning>("okx cascade detected: signals = {}, minutes = {}, active episodes = {}", count, kCascadeMinutes, m_active);

            for (size_t slot = 0; slot < kEpisodesCapacity; ++slot)
            {
                okx_close_episode(slot, "cascade");
            }

            continue;
        }

        okx_open_episode(signal);
    }

    m_log.flush();
}

void Strategy::okx_market() noexcept
{
    MarketEvent event{};
    while (m_okx_market.pop(event))
    {
        const Episode &episode = m_episodes[event.slot];

        const bool current = episode.stage != Stage::Idle && episode.id == event.episode;
        if (!current)
        {
            continue;
        }

        if (event.type == MarketEventType::Failure)
        {
            okx_close_episode(event.slot, "market");
            continue;
        }

        okx_episode_bar(event.slot, event.bar);
    }

    m_log.flush();
}

void Strategy::okx_account() noexcept
{
    AccountEvent event{};
    while (m_okx_account.pop(event))
    {
        if (event.type == AccountEventType::Balance)
        {
            m_balance = event.balance;
            m_balance_at = time::now_monotonic();

            continue;
        }

        if (event.type == AccountEventType::Disconnected)
        {
            m_balance_at = nanoseconds::zero();
            continue;
        }

        Episode &episode = m_episodes[event.slot];
        if (episode.id != event.episode) [[unlikely]]
        {
            continue;
        }

        if (event.type == AccountEventType::Released)
        {
            episode.executing = false;
            episode.closing = false;
            episode.held = Target::Flat;

            m_log.log("okx episode released: slot = {}, episode = {}", event.slot, event.episode);
            continue;
        }

        if (event.type == AccountEventType::Flat || (event.type == AccountEventType::Rejected && !event.closing))
        {
            episode.held = Target::Flat;
        }

        const bool restricted = event.type == AccountEventType::Rejected && event.code == 51155 && episode.stage != Stage::Idle;
        if (restricted) [[unlikely]]
        {
            Instrument &instrument = m_instruments[episode.instrument];
            instrument.restricted = true;

            m_log.log<logging::LogLevel::Warning>("okx {} is restricted for this account, episode closed, instrument skipped until the next refresh",
                                                  std::string_view(instrument.id));
            okx_close_episode(event.slot, "restricted");
        }

        if (event.type == AccountEventType::Flat)
        {
            episode.closing = false;
        }

        if (event.type == AccountEventType::Filled)
        {
            m_log.log("okx episode fill: slot = {}, episode = {}, side = {}, closing = {}, filled = {}, price = {}",
                      event.slot, event.episode, event.side == PositionSide::Long ? "long" : "short", event.closing, event.size, event.price);
        }
    }

    m_log.flush();
}

void Strategy::okx_open_episode(const ScreenerSignal &signal) noexcept
{
    const std::string_view id(signal.instrument);

    if (!m_okx_market.ready()) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx episode rejected: market clock is not ready, instrument = {}", id);
        return;
    }

    size_t slot = 0;
    for (; slot < kEpisodesCapacity; ++slot)
    {
        const Episode &episode = m_episodes[slot];

        const bool free = episode.stage == Stage::Idle && !episode.pending && !episode.executing && !episode.inferring;
        if (free)
        {
            break;
        }
    }

    if (slot == kEpisodesCapacity)
    {
        m_log.log<logging::LogLevel::Warning>("okx episode rejected: no free slot, instrument = {}", id);
        return;
    }

    const size_t index = find_instrument(id);
    if (index == m_instruments_size) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx episode rejected: unknown instrument = {}", id);
        return;
    }

    const Instrument &instrument = m_instruments[index];

    const Fee *const fee = find_fee(instrument.fee_group);
    if (fee == nullptr) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx episode rejected: unknown fee group = {}, instrument = {}", instrument.fee_group, id);
        return;
    }

    const nanoseconds now = time::now_monotonic();

    const nanoseconds age = now - m_balance_at;
    if (age > milliseconds(kBalanceMilliseconds)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx episode rejected: balance is stale, age = {} ms, instrument = {}", detail::ms(age), id);
        return;
    }

    const double capital = m_balance.equity - m_balance.isolated_pnl;

    double reserved = 0.0;
    for (const Episode &other : m_episodes)
    {
        reserved += other.stage != Stage::Idle ? other.budget : 0.0;
    }

    const double share = capital / static_cast<double>(kEpisodesCapacity);
    const double budget = std::min({ share, capital - reserved, m_balance.available });
    const double commission = std::max(0.0, -fee->taker);

    MinimumOrder order{};
    std::string_view rejection;

    const bool tradable = detail::fit_minimum_order(instrument, std::span<const Tier>(), budget, signal.close, commission,
                                                    static_cast<double>(kLeverage), order, rejection);
    if (!tradable)
    {
        m_log.log<logging::LogLevel::Warning>("okx episode rejected: not tradable, instrument = {}, reason = {}, budget = {}, margin = {}, fees = {}, leverage = {}",
                                              id, rejection, budget, order.margin, order.fees, order.leverage);
        return;
    }

    if (instrument.code == 0) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx episode rejected: instrument has no instIdCode, orders are impossible, instrument = {}", id);
        return;
    }

    if (instrument.restricted) [[unlikely]]
    {
        m_log.log("okx episode skipped: instrument is restricted for this account, instrument = {}", id);
        return;
    }

    Episode &episode = m_episodes[slot];
    episode = Episode{};
    episode.id = ++m_opened;
    episode.instrument = index;
    episode.opened = now;
    episode.budget = budget;
    episode.price = signal.close;
    episode.fee = commission;
    episode.leverage = order.leverage;
    episode.size = order.size;
    episode.stage = Stage::Tiers;

    m_histories[slot].clear();
    m_backlogs[slot] = Backlog{};
    std::fill_n(m_states[slot], kModelStateCapacity, 0.0f);
    ++m_active;

    m_log.log("okx episode opened: slot = {}, episode = {}, instrument = {}, price = {}, budget = {}, active = {}",
              slot, episode.id, id, signal.close, budget, m_active);
    m_log.log("okx {} contract: value = {}, multiplier = {}, lot = {}, min = {}, max limit = {}, tick = {}",
              id, instrument.contract_value, instrument.contract_multiplier, instrument.lot_size, instrument.min_size, instrument.max_limit_size, instrument.tick_size);

    const double contract = instrument.contract_value * instrument.contract_multiplier;
    if (!m_okx_market.subscribe(slot, episode.id, id, contract)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx {} market subscribe rejected: command queue is full", id);
        okx_close_episode(slot, "market");

        return;
    }

    if (!okx_episode_request(slot, Stage::Tiers)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx {} get position tiers request rejected by http client", id);
        okx_close_episode(slot, "tiers");
    }
}

void Strategy::okx_close_episode(size_t slot, std::string_view reason) noexcept
{
    Episode &episode = m_episodes[slot];
    if (episode.stage == Stage::Idle)
    {
        return;
    }

    const std::string_view id(m_instruments[episode.instrument].id);
    const nanoseconds lifetime = time::now_monotonic() - episode.opened;

    m_log.log("okx episode closed: slot = {}, episode = {}, instrument = {}, reason = {}, stage = {}, bars = {}, lifetime = {} ms",
              slot, episode.id, id, reason, stage_name(static_cast<uint8_t>(episode.stage)), m_histories[slot].size(), detail::ms(lifetime));

    if (!m_okx_market.unsubscribe(slot, episode.id)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx {} market unsubscribe rejected: command queue is full", id);
    }

    const bool executing = episode.executing;

    episode.stage = Stage::Idle;
    episode.budget = 0.0;
    episode.target = Target::Flat;

    m_backlogs[slot] = Backlog{};
    --m_active;

    if (executing && !okx_execute(slot, ExecutorCommandType::Release, Target::Flat)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx {} release not submitted: command queue is full, positions are closed on shutdown", id);
    }

    if (!episode.pending)
    {
        m_requests[slot].reset();
    }
}

void Strategy::okx_episode_response(size_t slot) noexcept
{
    Episode &episode = m_episodes[slot];
    if (!episode.pending) [[unlikely]]
    {
        return;
    }

    episode.pending = false;

    const network::HttpRequest &request = *m_requests[slot];

    if (episode.stage == Stage::Tiers)
    {
        okx_episode_tiers(slot, request);
    }
    else if (episode.stage == Stage::LeverageLong || episode.stage == Stage::LeverageShort)
    {
        okx_episode_leverage(slot, request);
    }
    else
    {
        m_requests[slot].reset();
    }

    m_log.flush();
}

void Strategy::okx_episode_tiers(size_t slot, const network::HttpRequest &request) noexcept
{
    Episode &episode = m_episodes[slot];
    const Instrument &instrument = m_instruments[episode.instrument];
    const std::string_view id(instrument.id);

    std::array<Tier, kTiersCapacity> tiers{};

    int code = -1;
    size_t count = 0;

    bool result = parsing::detail::okx_parse_tiers({ request.buffer, request.size }, code, tiers, count);
    result = result && request.code == 200 && code == 0 && count != 0;

    if (!result)
    {
        m_log.log<logging::LogLevel::Error>("okx {} get position tiers request failed: http = {}, code = {}, message = {}, tiers = {}", id, request.code, code, okx_message(request, code), count);
        okx_close_episode(slot, "tiers");

        return;
    }

    MinimumOrder order{};
    std::string_view rejection;

    const bool tradable = detail::fit_minimum_order(instrument, std::span(tiers).first(count), episode.budget, episode.price, episode.fee,
                                                    static_cast<double>(kLeverage), order, rejection);
    if (!tradable)
    {
        m_log.log<logging::LogLevel::Warning>("okx {} not tradable: reason = {}, budget = {}, margin = {}, fees = {}, leverage = {}",
                                              id, rejection, episode.budget, order.margin, order.fees, order.leverage);
        okx_close_episode(slot, "not tradable");

        return;
    }

    episode.leverage = order.leverage;
    episode.size = order.size;

    m_log.log("okx {} minimum order: leverage = {}, size = {}, notional = {}, margin = {}, fees = {}, budget = {}",
              id, order.leverage, order.size, order.notional, order.margin, order.fees, episode.budget);

    if (!okx_episode_request(slot, Stage::LeverageLong)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx {} set leverage request rejected by http client", id);
        okx_close_episode(slot, "leverage");
    }
}

void Strategy::okx_episode_leverage(size_t slot, const network::HttpRequest &request) noexcept
{
    Episode &episode = m_episodes[slot];
    const std::string_view id(m_instruments[episode.instrument].id);

    const bool longs = episode.stage == Stage::LeverageLong;
    const std::string_view side = longs ? "long" : "short";

    int code = -1;
    double leverage = 0.0;

    bool result = parsing::detail::okx_parse_leverage({ request.buffer, request.size }, code, leverage);
    result = result && request.code == 200 && code == 0;

    const bool throttled = !result && (code == 50011 || request.code == 429) && episode.retries < 3;
    if (throttled)
    {
        ++episode.retries;
        episode.retry = time::now_monotonic() + milliseconds(kRetryMilliseconds);

        m_log.log<logging::LogLevel::Warning>("okx {} set leverage throttled: side = {}, retry = {} in {} ms", id, side, episode.retries, kRetryMilliseconds);
        return;
    }

    if (!result)
    {
        m_log.log<logging::LogLevel::Error>("okx {} set leverage {} failed: side = {}, http = {}, code = {}, message = {}",
                                            id, episode.leverage, side, request.code, code, okx_message(request, code));
        okx_close_episode(slot, "leverage");

        return;
    }

    const bool applied = std::abs(leverage - episode.leverage) < 0.01;
    if (!applied) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx {} set leverage mismatch: side = {}, requested = {}, applied = {}", id, side, episode.leverage, leverage);
        okx_close_episode(slot, "leverage");

        return;
    }

    if (longs)
    {
        if (!okx_episode_request(slot, Stage::LeverageShort)) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Error>("okx {} set leverage request rejected by http client", id);
            okx_close_episode(slot, "leverage");
        }

        return;
    }

    episode.stage = Stage::Warmup;
    m_requests[slot].reset();

    const nanoseconds elapsed = time::now_monotonic() - episode.opened;

    m_log.log("okx episode prepared: slot = {}, instrument = {}, leverage = {}, bars = {}, elapsed = {} ms",
              slot, id, episode.leverage, m_histories[slot].size(), detail::ms(elapsed));
}

void Strategy::okx_episode_bar(size_t slot, const Bar &bar) noexcept
{
    Episode &episode = m_episodes[slot];
    RingBuffer<Bar, kHistoryBars> &history = m_histories[slot];

    const std::string_view id(m_instruments[episode.instrument].id);

    const bool continuous = history.size() == 0 || bar.timestamp == history.back().timestamp + 1000;
    if (!continuous) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx {} bars are not continuous: last = {}, timestamp = {}", id, history.back().timestamp, bar.timestamp);
        okx_close_episode(slot, "bars");

        return;
    }

    history.push(bar);

    m_bar_delay.record(time::now() - milliseconds(bar.timestamp + 1000));

    Backlog &backlog = m_backlogs[slot];
    if (m_model.ready())
    {
        if (backlog.size == kPendingBars) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Warning>("okx {} model backlog is full, the oldest bar is skipped", id);

            backlog.head = (backlog.head + 1) % kPendingBars;
            --backlog.size;
        }

        backlog.bars[(backlog.head + backlog.size) % kPendingBars] = bar;
        ++backlog.size;

        if (!episode.inferring)
        {
            okx_infer(slot);
        }
    }

    if (history.size() == 1)
    {
        m_log.log("okx {} first bar: timestamp = {}, close = {}, bid = {}, ask = {}, volume = {}", id, bar.timestamp, bar.close, bar.bid, bar.ask, bar.volume);
    }

    const nanoseconds elapsed = time::now_monotonic() - episode.opened;

    const bool warmed = episode.stage == Stage::Warmup && history.full() && elapsed >= minutes(kWarmupMinutes);
    if (!warmed)
    {
        return;
    }

    episode.stage = Stage::Ready;

    const std::span<const Bar, kHistoryBars> window = history.history();

    double volume = 0.0;
    for (const Bar &item : window)
    {
        volume += item.volume;
    }

    m_log.log("okx episode ready: slot = {}, instrument = {}, first = {}, last = {}, close = {}, volume = {}, elapsed = {} ms",
              slot, id, window.front().timestamp, window.back().timestamp, bar.close, volume, detail::ms(elapsed));
}

bool Strategy::okx_episode_request(size_t slot, Stage stage) noexcept
{
    static_assert(kEpisodeRequestCapacity > parsing::json_padding_size);

    Episode &episode = m_episodes[slot];
    const Instrument &instrument = m_instruments[episode.instrument];

    episode.stage = stage;

    network::HttpRequest &request = m_requests[slot].emplace(m_buffers[slot], kEpisodeRequestCapacity - parsing::json_padding_size, 0, 0, false,
                                                             m_okx_episodes_notify[slot]);

    bool pushed = false;
    if (stage == Stage::Tiers)
    {
        pushed = m_okx_http_client.get_position_tiers(request, "SWAP", "isolated", instrument.family);
    }
    else
    {
        const std::string_view side = stage == Stage::LeverageLong ? "long" : "short";
        pushed = m_okx_http_client.set_leverage(request, instrument.id, episode.leverage, side);
    }

    episode.pending = pushed;

    return pushed;
}

void Strategy::okx_infer(size_t slot) noexcept
{
    Episode &episode = m_episodes[slot];
    Backlog &backlog = m_backlogs[slot];

    if (backlog.size == 0 || episode.inferring)
    {
        return;
    }

    const Bar &bar = backlog.bars[backlog.head];
    backlog.head = (backlog.head + 1) % kPendingBars;
    --backlog.size;

    const std::array<double, 7> fields = { bar.open, bar.high, bar.low, bar.close, bar.volume, bar.bid, bar.ask };

    const size_t size = m_model.input_size();
    for (size_t index = 0; index < size; ++index)
    {
        m_inputs[slot][index] = index < fields.size() ? static_cast<float>(fields[index]) : 0.0f;
    }

    const nanoseconds now = time::now_monotonic();

    const ModelRequest request{ m_inputs[slot], m_states[slot], m_outputs[slot], &m_model_notify[slot], now };
    if (!m_model.submit(request)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx model request rejected: slot = {}, queue is full", slot);
        return;
    }

    episode.inferring = true;
    episode.submitted = now;
}

void Strategy::okx_inferred(size_t slot) noexcept
{
    Episode &episode = m_episodes[slot];
    if (!episode.inferring) [[unlikely]]
    {
        return;
    }

    episode.inferring = false;
    m_model_trip.record(time::now_monotonic() - episode.submitted);

    if (episode.stage == Stage::Idle)
    {
        m_backlogs[slot] = Backlog{};
        return;
    }

    if (episode.stage == Stage::Ready && m_backlogs[slot].size == 0)
    {
        okx_decide(slot);
    }

    okx_infer(slot);

    m_log.flush();
}

void Strategy::okx_decide(size_t slot) noexcept
{
    Episode &episode = m_episodes[slot];

    const float *const output = m_outputs[slot];
    const size_t size = std::min<size_t>(m_model.output_size(), 3);

    size_t best = 0;
    for (size_t index = 1; index < size; ++index)
    {
        best = output[index] > output[best] ? index : best;
    }

    constexpr std::array<Target, 3> targets = { Target::Long, Target::Flat, Target::Short };
    const Target target = size == 1 ? (output[0] > 0.0f ? Target::Long : Target::Short) : targets[best];

    const nanoseconds now = time::now_monotonic();

    if (target != episode.target)
    {
        m_log.log("okx episode target: slot = {}, episode = {}, target = {}, held = {}, outputs = {} {} {}", slot, episode.id,
                  static_cast<int>(target), static_cast<int>(episode.held), output[0], size > 1 ? output[1] : 0.0f, size > 2 ? output[2] : 0.0f);
        episode.target = target;
    }

    const bool settled = episode.acted == nanoseconds::zero() || now - episode.acted >= seconds(kDecisionSeconds);
    if (!settled)
    {
        return;
    }

    const milliseconds closed(m_histories[slot].back().timestamp + 1000);

    if (episode.closing)
    {
        return;
    }

    if (episode.held != Target::Flat && episode.held != target)
    {
        if (okx_execute(slot, ExecutorCommandType::Close, episode.held))
        {
            episode.closing = true;
            episode.acted = now;
            m_decision_delay.record(time::now() - closed);
        }

        return;
    }

    if (episode.held == Target::Flat && target != Target::Flat)
    {
        if (okx_execute(slot, ExecutorCommandType::Open, target))
        {
            episode.held = target;
            episode.acted = now;
            m_decision_delay.record(time::now() - closed);
        }
    }
}

bool Strategy::okx_execute(size_t slot, ExecutorCommandType type, Target target) noexcept
{
    Episode &episode = m_episodes[slot];
    const Instrument &instrument = m_instruments[episode.instrument];

    ExecutorCommand command{};
    nlx::detail::str_assign(command.instrument, std::string_view(instrument.id));
    command.slot = slot;
    command.episode = episode.id;
    command.code = instrument.code;
    command.lot = instrument.lot_size;
    command.tick = instrument.tick_size;
    command.type = type;
    command.side = target == Target::Short ? PositionSide::Short : PositionSide::Long;
    command.order = OrderType::Market;
    command.size = episode.size;
    command.submitted = time::now_monotonic();

    if (!m_okx_account.submit(command)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx {} executor command rejected: queue is full", std::string_view(instrument.id));
        return false;
    }

    episode.executing = true;

    m_log.log("okx episode command: slot = {}, episode = {}, type = {}, side = {}, size = {}, price = {}",
              slot, episode.id, static_cast<int>(type), static_cast<int>(target), command.size, command.price);

    return true;
}

size_t Strategy::find_instrument(std::string_view id) const noexcept
{
    for (size_t index = 0; index < m_instruments_size; ++index)
    {
        if (std::string_view(m_instruments[index].id) == id)
        {
            return index;
        }
    }

    return m_instruments_size;
}

const Fee *Strategy::find_fee(int group) const noexcept
{
    for (size_t index = 0; index < m_fees_size; ++index)
    {
        if (m_fees[index].group == group)
        {
            return &m_fees[index];
        }
    }

    return nullptr;
}

bool Strategy::okx_reinstruments() noexcept
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

    std::array<Instrument, kInstrumentsCapacity> instruments{};

    int code = -1;
    size_t listed = 0;
    size_t ignored = 0;

    bool result = okx_get_instruments(request, code, since, instruments, "SWAP", "linear", "USDT", "1", listed, ignored);
    if (!result)
    {
        m_log.log<logging::LogLevel::Error>("okx get instruments request failed: http = {}, code = {}, message = {}, size = {}, truncated = {}",
                                            request.code, code, okx_message(request, code), request.size, request.truncated);
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

    std::span<Instrument> listed_instruments = std::span(instruments).first(listed);

    result = okx_load_liquidity(listed_instruments, today, detail::ceil_milliseconds(now), valid, liquid);
    if (m_stopping.load(std::memory_order_acquire)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx instruments refresh interrupted: stop requested");
        m_log.flush();

        return false;
    }

    if (!result)
    {
        return false;
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

    std::copy_n(instruments.begin(), listed, m_instruments.begin());
    m_instruments_size = listed;

    result = m_okx_screener.resubscribe(listed_instruments);
    if (!result) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx instruments resubscribe failed: instruments = {}", liquid);
        m_log.flush();

        return false;
    }

    m_log.log("okx instruments resubscribed: instruments = {}", liquid);
    m_log.flush();

    return true;
}

bool Strategy::okx_load_liquidity(std::span<Instrument> instruments, milliseconds today, milliseconds before, size_t &valid, size_t &liquid) noexcept
{
    char buffers[kCandlesSlots][kCandlesCapacity];
    static_assert(kCandlesCapacity > parsing::json_padding_size);

    std::array<std::optional<network::HttpRequest>, kCandlesSlots> requests{};
    std::array<size_t, kCandlesSlots> owners{};

    size_t next = 0;
    size_t active = 0;
    size_t retries = 0;
    bool failed = false;

    while (true)
    {
        while (active < kCandlesSlots && next < instruments.size() && !failed)
        {
            if (m_stopping.load(std::memory_order_acquire)) [[unlikely]]
            {
                break;
            }

            size_t slot = 0;
            while (requests[slot].has_value())
            {
                ++slot;
            }

            network::HttpRequest &request = requests[slot].emplace(buffers[slot], kCandlesCapacity - parsing::json_padding_size, 0, 0, false, m_okx_candles_notify[slot]);
            const std::string_view id(instruments[next].id);

            const bool pushed = m_okx_http_client.get_candles(request, id, "1Dutc", before, static_cast<uint32_t>(kLiquidityDays + 1));
            if (!pushed) [[unlikely]]
            {
                m_log.log<logging::LogLevel::Error>("okx {} get candles request rejected by http client", id);
                m_log.flush();

                requests[slot].reset();
                failed = true;

                break;
            }

            owners[slot] = next;

            ++next;
            ++active;
        }

        if (active == 0)
        {
            break;
        }

        epoll_event event{};
        const size_t count = detail::epoll_wait(m_okx_candles_epoll, { &event, 1 }, -1);
        if (count == 0)
        {
            continue;
        }

        size_t slot = 0;
        while (m_okx_candles_notify[slot] != event.data.fd)
        {
            ++slot;
        }

        [[maybe_unused]] const uint64_t result = detail::drain(m_okx_candles_notify[slot]);

        const bool throttled = !failed && requests[slot]->code == 429 && retries < 32;
        if (throttled)
        {
            ++retries;

            const std::string_view id(instruments[owners[slot]].id);
            network::HttpRequest &request = requests[slot].emplace(buffers[slot], kCandlesCapacity - parsing::json_padding_size, 0, 0, false, m_okx_candles_notify[slot]);

            if (m_okx_http_client.get_candles(request, id, "1Dutc", before, static_cast<uint32_t>(kLiquidityDays + 1)))
            {
                continue;
            }

            m_log.log<logging::LogLevel::Error>("okx {} get candles retry rejected by http client", id);
            failed = true;
        }

        if (!failed)
        {
            failed = !okx_check_liquidity(*requests[slot], instruments[owners[slot]], today, valid, liquid);
        }

        requests[slot].reset();
        --active;
    }

    if (retries != 0) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx liquidity candles throttled by the exchange and retried: retries = {}", retries);
        m_log.flush();
    }

    return !failed;
}

bool Strategy::okx_check_liquidity(const network::HttpRequest &request, Instrument &instrument, milliseconds today, size_t &valid, size_t &liquid) noexcept
{
    const std::string_view id(instrument.id);

    std::array<Volume, kLiquidityDays + 1> daily{};

    int code = -1;
    size_t count = 0;

    bool result = parsing::detail::okx_parse_candles({ request.buffer, request.size }, code, daily, count);
    result = result && request.code == 200 && code == 0;

    if (!result)
    {
        if (request.code != 200 || code <= 0)
        {
            m_log.log<logging::LogLevel::Error>("okx {} get candles request failed: http = {}, code = {}, message = {}", id, request.code, code, okx_message(request, code));
            m_log.flush();

            return false;
        }

        m_log.log<logging::LogLevel::Warning>("okx {} get candles rejected: code = {}", id, code);
        return true;
    }

    const size_t offset = count != 0 && milliseconds(daily[0].timestamp) == today ? 1 : 0;
    if (count < offset + kLiquidityDays)
    {
        m_log.log<logging::LogLevel::Warning>("okx {} get candles rejected: days = {}", id, count - offset);
        return true;
    }

    const std::span<const Volume, kLiquidityDays> closed(daily.data() + offset, kLiquidityDays);

    std::array<double, kLiquidityDays> volumes{};

    bool confirmed = true;
    bool consecutive = true;
    for (size_t index = 0; index < kLiquidityDays; ++index)
    {
        const Volume &volume = closed[index];

        confirmed = confirmed && volume.confirmed;
        consecutive = consecutive && milliseconds(volume.timestamp) == today - days(index + 1);
        volumes[index] = volume.volume;
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
        return true;
    }

    const bool liquid_pass = liquid_filter(volumes);
    instrument.accepted = liquid_pass;

    liquid += static_cast<size_t>(liquid_pass);
    ++valid;

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

bool Strategy::okx_get_instruments(network::HttpRequest &request, int &code, milliseconds listed_before, std::span<Instrument> instruments,
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

bool Strategy::okx_get_balance(network::HttpRequest &request, int &code, std::string_view currency, Balance &balance) noexcept
{
    code = -1;
    bool result = m_okx_http_client.get_balance(request, currency);
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_balance({ request.buffer, request.size }, code, currency, balance);

    return result && request.code == 200 && code == 0;
}

bool Strategy::okx_get_fees(network::HttpRequest &request, int &code, std::span<Fee> fees, size_t &count) noexcept
{
    code = -1;
    bool result = m_okx_http_client.get_trade_fee(request, "SWAP");
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_fees({ request.buffer, request.size }, code, fees, count);

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
