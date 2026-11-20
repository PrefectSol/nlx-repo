#include "service/account.h"
#include "core/fatal.h"
#include "exchange/okx_account_streamer.h"

#include <chrono>
#include <string_view>

namespace nlx::service
{

template<typename Streamer>
Account<Streamer>::Account(const Credentials &credentials, exchange::OkxRateLimiter &limiter, const char *url, logging::LoggerRegistry &registry) :
    m_log(),
    m_streamer(credentials, limiter, url, m_log),
    m_executor(m_streamer, m_log, static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(time::now()).count() % 1000000)),
    m_epoll(),
    m_stop(),
    m_command(),
    m_timer(),
    m_event(),
    m_initialized(false),
    m_commands(),
    m_events(),
    m_balance(),
    m_connected(false),
    m_loop(),
    m_receive(),
    m_command_delay(),
    m_ticks(0)
{
    m_log = registry.create_node("okx_account");

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
void Account<Streamer>::initialize()
{
    m_streamer.initialize();

    m_initialized.store(true, std::memory_order_release);
}

template<typename Streamer>
void Account<Streamer>::start() noexcept
{
    if (!m_initialized.load(std::memory_order_acquire)) [[unlikely]]
    {
        NLX_FATAL("nlx::service::Account::start() called before initialize()");
    }

    m_log.log("account started");
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

    drain();

    m_streamer.close();

    m_log.log("account stopped");
    m_log.flush();
}

template<typename Streamer>
void Account<Streamer>::request_stop() noexcept
{
    detail::notify(m_stop);
}

template<typename Streamer>
bool Account<Streamer>::submit(const ExecutorCommand &command) noexcept
{
    if (!m_commands.push(command)) [[unlikely]]
    {
        return false;
    }

    detail::notify(m_command);

    return true;
}

template<typename Streamer>
const FileDescriptor &Account<Streamer>::events() const noexcept
{
    return m_event;
}

template<typename Streamer>
bool Account<Streamer>::pop(AccountEvent &event) noexcept
{
    return m_events.pop(event);
}

template<typename Streamer>
void Account<Streamer>::receive() noexcept
{
    AccountUpdateType type{};
    while (m_streamer.poll(type))
    {
        if (type == AccountUpdateType::Balance)
        {
            balance();
        }
        else if (type == AccountUpdateType::Positions)
        {
            positions();
        }
        else if (type == AccountUpdateType::Orders)
        {
            orders();
        }
        else
        {
            m_executor.reply(m_streamer.reply(), time::now_monotonic());
        }
    }

    const bool connected = m_streamer.connected();
    if (m_connected && !connected) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx account stream lost: balance is stale until reconnect");

        AccountEvent event{};
        event.type = AccountEventType::Disconnected;
        emit(event);
    }

    m_connected = connected;

    forward();
    m_log.flush();
}

template<typename Streamer>
void Account<Streamer>::command() noexcept
{
    ExecutorCommand command{};
    while (m_commands.pop(command))
    {
        const nanoseconds now = time::now_monotonic();
        m_command_delay.record(now - command.submitted);

        m_executor.command(command, now);
    }

    forward();
    m_log.flush();
}

template<typename Streamer>
void Account<Streamer>::tick() noexcept
{
    m_streamer.heartbeat();
    receive();

    m_executor.tick(time::now_monotonic());

    forward();

    ++m_ticks;
    if (m_ticks % kMetricsTicks == 0)
    {
        const nanoseconds now = time::now_monotonic();

        m_log.log<logging::LogLevel::Metric>("account metrics: utilization = {} %, wakeups = {}, receive p50 = {} us, p99 = {} us, max = {} us, command queue p99 = {} us",
                  m_loop.utilization(now), m_loop.wakeups(), m_receive.percentile(0.5), m_receive.percentile(0.99), m_receive.max(),
                  m_command_delay.percentile(0.99));
        m_executor.report();

        m_loop.reset(now);
        m_receive.reset();
        m_command_delay.reset();
    }

    m_log.flush();
}

template<typename Streamer>
void Account<Streamer>::drain() noexcept
{
    const nanoseconds begin = time::now_monotonic();
    m_executor.drain(begin);

    epoll_event event{};
    while (!m_executor.flat())
    {
        const nanoseconds elapsed = time::now_monotonic() - begin;
        if (elapsed >= milliseconds(kDrainMilliseconds)) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Error>("okx account drain timed out after {} ms: positions or orders may remain, check the exchange", detail::ms(elapsed));
            m_log.flush();

            return;
        }

        const size_t count = detail::epoll_wait(m_epoll, { &event, 1 }, static_cast<int>(kTimerMilliseconds));

        const bool streamed = count != 0 && event.data.fd == m_streamer.fd();
        if (streamed)
        {
            receive();
            continue;
        }

        if (count != 0 && event.data.fd == m_timer)
        {
            [[maybe_unused]] const uint64_t ticks = detail::drain(m_timer);
        }

        if (count != 0 && event.data.fd == m_command)
        {
            [[maybe_unused]] const uint64_t result = detail::drain(m_command);
        }

        tick();
    }

    m_log.log("okx account drained in {} ms: no orders, no positions", detail::ms(time::now_monotonic() - begin));
    m_log.flush();
}

template<typename Streamer>
void Account<Streamer>::balance() noexcept
{
    const Balance &balance = m_streamer.balance();

    const bool changed = balance.equity != m_balance.equity || balance.available != m_balance.available || balance.isolated_pnl != m_balance.isolated_pnl;
    if (changed)
    {
        m_log.log("okx account balance: equity = {}, available = {}, isolated pnl = {}", balance.equity, balance.available, balance.isolated_pnl);
        m_balance = balance;
    }

    AccountEvent event{};
    event.type = AccountEventType::Balance;
    event.balance = balance;
    event.timestamp = m_streamer.timestamp();

    emit(event);
}

template<typename Streamer>
void Account<Streamer>::positions() noexcept
{
    const std::span<const Position> positions = m_streamer.positions();

    for (const Position &position : positions)
    {
        m_log.log("okx account position: instrument = {}, side = {}, size = {}, price = {}, liquidation = {}, margin = {}, pnl = {}",
                  std::string_view(position.instrument), std::string_view(position.side), position.size, position.price,
                  position.liquidation, position.margin, position.pnl);
    }

    m_executor.positions(positions, m_streamer.snapshot(), time::now_monotonic());
}

template<typename Streamer>
void Account<Streamer>::orders() noexcept
{
    const std::span<const Order> orders = m_streamer.orders();

    for (const Order &order : orders)
    {
        m_log.log("okx account order: instrument = {}, client = {}, side = {}, position = {}, state = {}, size = {}, filled = {}, price = {}",
                  std::string_view(order.instrument), std::string_view(order.client), std::string_view(order.side), std::string_view(order.position),
                  std::string_view(order.state), order.size, order.filled, order.price);
    }

    m_executor.orders(orders, time::now_monotonic());
}

template<typename Streamer>
void Account<Streamer>::forward() noexcept
{
    AccountEvent event{};
    while (m_executor.pop(event))
    {
        emit(event);
    }
}

template<typename Streamer>
void Account<Streamer>::emit(const AccountEvent &event) noexcept
{
    if (!m_events.push(event)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx account event dropped: queue is full");
        return;
    }

    detail::notify(m_event);
}

template class Account<exchange::OkxAccountStreamer>;

} // namespace nlx::service
