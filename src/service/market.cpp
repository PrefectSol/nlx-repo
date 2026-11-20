#include "service/market.h"
#include "core/fatal.h"
#include "core/string.h"
#include "exchange/okx_market_streamer.h"

#include <algorithm>

namespace nlx::service
{

template<typename Streamer>
Market<Streamer>::Market(exchange::OkxRateLimiter &limiter, const char *url, size_t clock_milliseconds, logging::LoggerRegistry &registry) :
    m_log(),
    m_streamer(limiter, url, clock_milliseconds, m_log),
    m_epoll(),
    m_stop(),
    m_command(),
    m_timer(),
    m_event(),
    m_initialized(false),
    m_ready(false),
    m_commands(),
    m_events(),
    m_slots(),
    m_second(0),
    m_ticks(0),
    m_loop(),
    m_receive(),
    m_delivery(),
    m_close(),
    m_updates(0)
{
    m_log = registry.create_node("okx_market");

    m_epoll = FileDescriptor::make_epoll();
    m_stop = FileDescriptor::make_eventfd(true);
    m_command = FileDescriptor::make_eventfd(true);
    m_timer = FileDescriptor::make_timerfd(milliseconds(kTimerMilliseconds), true);
    m_event = FileDescriptor::make_eventfd(true);

    detail::epoll_add(m_epoll, m_stop);
    detail::epoll_add(m_epoll, m_command);
    detail::epoll_add(m_epoll, m_timer);
    detail::epoll_add(m_epoll, m_streamer.fd());
}

template<typename Streamer>
void Market<Streamer>::initialize()
{
    m_streamer.initialize();

    m_initialized.store(true, std::memory_order_release);
}

template<typename Streamer>
void Market<Streamer>::start() noexcept
{
    if (!m_initialized.load(std::memory_order_acquire)) [[unlikely]]
    {
        NLX_FATAL("nlx::service::Market::start() called before initialize()");
    }

    m_log.log("market started");
    m_log.log("market slots: {}, snapshot timeout = {} ms", capacity(), kReadyMilliseconds);
    m_log.flush();

    m_streamer.heartbeat();
    receive();

    m_loop.reset(time::now_monotonic());

    epoll_event event{};
    while (true)
    {
        m_loop.sleep(time::now_monotonic());
        const size_t count = detail::epoll_wait(m_epoll, { &event, 1 }, -1);
        m_loop.wake(time::now_monotonic());

        if (count == 0)
        {
            continue;
        }

        if (event.data.fd == m_streamer.fd()) [[likely]]
        {
            const nanoseconds begin = time::now_monotonic();
            receive();
            m_receive.record(time::now_monotonic() - begin);
        }
        else if (event.data.fd == m_command)
        {
            [[maybe_unused]] const uint64_t result = detail::drain(m_command);
            command();
        }
        else if (event.data.fd == m_timer)
        {
            [[maybe_unused]] const uint64_t ticks = detail::drain(m_timer);
            tick();
        }
        else if (event.data.fd == m_stop)
        {
            [[maybe_unused]] const uint64_t result = detail::drain(m_stop);
            break;
        }
    }

    m_streamer.close();

    m_log.log("market stopped");
    m_log.flush();
}

template<typename Streamer>
void Market<Streamer>::request_stop() noexcept
{
    detail::notify(m_stop);
}

template<typename Streamer>
bool Market<Streamer>::subscribe(size_t slot, uint64_t episode, std::string_view instrument, double contract) noexcept
{
    Command command{};
    nlx::detail::str_assign(command.instrument, instrument);
    command.slot = slot;
    command.episode = episode;
    command.contract = contract;
    command.subscribe = true;

    if (!m_commands.push(command)) [[unlikely]]
    {
        return false;
    }

    detail::notify(m_command);

    return true;
}

template<typename Streamer>
bool Market<Streamer>::unsubscribe(size_t slot, uint64_t episode) noexcept
{
    Command command{};
    command.slot = slot;
    command.episode = episode;
    command.subscribe = false;

    if (!m_commands.push(command)) [[unlikely]]
    {
        return false;
    }

    detail::notify(m_command);

    return true;
}

template<typename Streamer>
const FileDescriptor &Market<Streamer>::events() const noexcept
{
    return m_event;
}

template<typename Streamer>
bool Market<Streamer>::pop(MarketEvent &event) noexcept
{
    return m_events.pop(event);
}

template<typename Streamer>
bool Market<Streamer>::ready() const noexcept
{
    return m_ready.load(std::memory_order_acquire);
}

template<typename Streamer>
void Market<Streamer>::command() noexcept
{
    Command command{};
    while (m_commands.pop(command))
    {
        if (command.subscribe)
        {
            open(command);
            continue;
        }

        const Slot &slot = m_slots[command.slot];
        if (slot.active && slot.episode == command.episode)
        {
            release(command.slot, "stopped");
        }
    }

    m_log.flush();
}

template<typename Streamer>
void Market<Streamer>::tick() noexcept
{
    m_streamer.heartbeat();
    receive();

    const nanoseconds now = time::now_monotonic();

    for (size_t index = 0; index < m_slots.size(); ++index)
    {
        Slot &slot = m_slots[index];

        const bool stalled = slot.active && slot.first == 0 && now - slot.started >= milliseconds(kReadyMilliseconds);
        if (stalled) [[unlikely]]
        {
            fail(index, "snapshot timeout");
        }

        if (slot.active && slot.late != slot.reported) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Warning>("okx market late trades dropped: slot = {}, instrument = {}, late = {}",
                                                  index, std::string_view(slot.instrument), slot.late);
            slot.reported = slot.late;
        }
    }

    ++m_ticks;
    if (m_ticks % kMetricsTicks == 0)
    {
        report();
    }

    m_log.flush();
}

template<typename Streamer>
void Market<Streamer>::report() noexcept
{
    const nanoseconds now = time::now_monotonic();

    m_log.log<logging::LogLevel::Metric>("market metrics: utilization = {} %, wakeups = {}, updates = {}, receive p50 = {} us, p99 = {} us, max = {} us",
              m_loop.utilization(now), m_loop.wakeups(), m_updates, m_receive.percentile(0.5), m_receive.percentile(0.99), m_receive.max());
    m_log.log<logging::LogLevel::Metric>("market metrics: delivery p50 = {} ms, p99 = {} ms, bar close p50 = {} ms, p99 = {} ms, max = {} ms, bars = {}",
              m_delivery.percentile(0.5) / 1000.0, m_delivery.percentile(0.99) / 1000.0, m_close.percentile(0.5) / 1000.0,
              m_close.percentile(0.99) / 1000.0, m_close.max() / 1000.0, m_close.count());

    m_loop.reset(now);
    m_receive.reset();
    m_delivery.reset();
    m_close.reset();
    m_updates = 0;
}

template<typename Streamer>
void Market<Streamer>::receive() noexcept
{
    MarketUpdate update{};
    while (m_streamer.poll(update))
    {
        if (update.type == MarketUpdateType::Failure) [[unlikely]]
        {
            fail(update.slot, "feed");
            continue;
        }

        if (update.type == MarketUpdateType::Limit)
        {
            Slot &limited = m_slots[update.slot];
            limited.buy_limit = update.price;
            limited.sell_limit = update.size;

            continue;
        }

        ++m_updates;
        m_delivery.record(time::now() - milliseconds(update.timestamp));

        advance(update.timestamp);

        if (update.type == MarketUpdateType::Clock)
        {
            continue;
        }

        Slot &slot = m_slots[update.slot];
        if (!slot.active) [[unlikely]]
        {
            continue;
        }

        if (update.type == MarketUpdateType::Book)
        {
            quote(update.slot, update.timestamp);
            continue;
        }

        trade(slot, update);
    }

    synchronize();

    m_log.flush();
}

template<typename Streamer>
void Market<Streamer>::synchronize() noexcept
{
    const bool alive = m_streamer.alive();
    if (alive == m_ready.load(std::memory_order_relaxed))
    {
        return;
    }

    m_ready.store(alive, std::memory_order_release);

    if (alive)
    {
        m_log.log("okx market clock ready: episodes can be opened");
        return;
    }

    m_log.log<logging::LogLevel::Warning>("okx market clock lost: episodes cannot be opened");
}

template<typename Streamer>
void Market<Streamer>::open(const Command &command) noexcept
{
    const std::string_view instrument(command.instrument);

    if (m_slots[command.slot].active) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx market slot replaced: slot = {}, instrument = {}", command.slot, instrument);
        release(command.slot, "replaced");
    }

    Slot &slot = m_slots[command.slot];
    nlx::detail::str_assign(slot.instrument, instrument);
    slot.episode = command.episode;
    slot.contract = command.contract;
    slot.bar = Bar{};
    slot.bid = 0.0;
    slot.ask = 0.0;
    slot.buy_limit = 0.0;
    slot.sell_limit = 0.0;
    slot.first = 0;
    slot.bars = 0;
    slot.late = 0;
    slot.reported = 0;
    slot.started = time::now_monotonic();
    slot.traded = false;
    slot.priced = false;

    if (!m_streamer.subscribe(command.slot, instrument)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx market subscribe failed: slot = {}, episode = {}, instrument = {}", command.slot, command.episode, instrument);
        emit(MarketEventType::Failure, command.slot);

        return;
    }

    slot.active = true;

    m_log.log("okx market subscribed: slot = {}, episode = {}, instrument = {}, contract = {}", command.slot, command.episode, instrument, command.contract);
}

template<typename Streamer>
void Market<Streamer>::release(size_t index, std::string_view reason) noexcept
{
    Slot &slot = m_slots[index];
    slot.active = false;

    m_streamer.unsubscribe(index);

    m_log.log("okx market unsubscribed: slot = {}, episode = {}, instrument = {}, reason = {}, bars = {}, late = {}",
              index, slot.episode, std::string_view(slot.instrument), reason, slot.bars, slot.late);
}

template<typename Streamer>
void Market<Streamer>::fail(size_t index, std::string_view reason) noexcept
{
    if (!m_slots[index].active)
    {
        return;
    }

    m_log.log<logging::LogLevel::Warning>("okx market failed: slot = {}, instrument = {}, reason = {}", index, std::string_view(m_slots[index].instrument), reason);

    emit(MarketEventType::Failure, index);
    release(index, reason);
}

template<typename Streamer>
void Market<Streamer>::advance(int64_t timestamp) noexcept
{
    const int64_t second = timestamp / 1000;

    if (m_second == 0)
    {
        m_second = second;
        return;
    }

    if (second - m_second > kGapSeconds) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx market clock jumped: seconds = {}", second - m_second);

        m_second = second;
        return;
    }

    const bool crossed = m_second < second;

    while (m_second < second)
    {
        finish(m_second);
        ++m_second;
    }

    if (crossed)
    {
        m_close.record(time::now() - seconds(second));
    }
}

template<typename Streamer>
void Market<Streamer>::finish(int64_t second) noexcept
{
    for (size_t index = 0; index < m_slots.size(); ++index)
    {
        Slot &slot = m_slots[index];

        const bool open = slot.active && slot.first != 0 && second >= slot.first;
        if (!open)
        {
            continue;
        }

        Bar &bar = slot.bar;
        if (!slot.traded)
        {
            const double price = slot.priced ? bar.close : (slot.bid + slot.ask) / 2.0;

            bar.open = price;
            bar.high = price;
            bar.low = price;
            bar.close = price;
            bar.volume = 0.0;

            slot.priced = true;
        }

        bar.timestamp = second * 1000;
        bar.bid = slot.bid;
        bar.ask = slot.ask;
        bar.buy_limit = slot.buy_limit;
        bar.sell_limit = slot.sell_limit;

        emit(MarketEventType::Bar, index);

        slot.traded = false;
        ++slot.bars;
    }
}

template<typename Streamer>
void Market<Streamer>::quote(size_t index, int64_t timestamp) noexcept
{
    Slot &slot = m_slots[index];
    slot.bid = m_streamer.bid(index);
    slot.ask = m_streamer.ask(index);

    if (slot.first != 0)
    {
        return;
    }

    slot.first = timestamp / 1000 + 1;

    const nanoseconds elapsed = time::now_monotonic() - slot.started;

    m_log.log("okx market ready: slot = {}, instrument = {}, first = {}, bid = {}, ask = {}, elapsed = {} ms",
              index, std::string_view(slot.instrument), slot.first * 1000, slot.bid, slot.ask, detail::ms(elapsed));
}

template<typename Streamer>
void Market<Streamer>::trade(Slot &slot, const MarketUpdate &update) noexcept
{
    const int64_t second = update.timestamp / 1000;

    if (slot.first == 0 || second < slot.first)
    {
        return;
    }

    if (second < m_second) [[unlikely]]
    {
        ++slot.late;
        return;
    }

    Bar &bar = slot.bar;
    if (!slot.traded)
    {
        bar.open = update.price;
        bar.high = update.price;
        bar.low = update.price;
        bar.volume = 0.0;

        slot.traded = true;
        slot.priced = true;
    }

    bar.high = std::max(bar.high, update.price);
    bar.low = std::min(bar.low, update.price);
    bar.close = update.price;
    bar.volume += update.price * update.size * slot.contract;
}

template<typename Streamer>
void Market<Streamer>::emit(MarketEventType type, size_t index) noexcept
{
    const Slot &slot = m_slots[index];

    const MarketEvent event{ type, index, slot.episode, slot.bar };
    if (!m_events.push(event)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx market event dropped: queue is full, slot = {}, instrument = {}", index, std::string_view(slot.instrument));
        return;
    }

    detail::notify(m_event);
}

template class Market<exchange::OkxMarketStreamer>;

} // namespace nlx::service
