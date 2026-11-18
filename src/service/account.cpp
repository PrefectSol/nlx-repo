#include "service/account.h"
#include "core/fatal.h"
#include "exchange/okx_account_streamer.h"

#include <string_view>

namespace nlx::service
{

template<typename Streamer>
Account<Streamer>::Account(const Credentials &credentials, exchange::OkxRateLimiter &limiter, logging::LoggerRegistry &registry) :
    m_log(),
    m_streamer(credentials, limiter, m_log),
    m_epoll(),
    m_stop(),
    m_timer(),
    m_event(),
    m_initialized(false),
    m_events(),
    m_balance(),
    m_connected(false)
{
    m_log = registry.create_node("okx_account");

    m_epoll = FileDescriptor::make_epoll();
    m_stop = FileDescriptor::make_eventfd(true);
    m_timer = FileDescriptor::make_timerfd(milliseconds(kTimerMilliseconds), true);
    m_event = FileDescriptor::make_eventfd(true);

    detail::epoll_add(m_epoll, m_stop);
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

    epoll_event event{};
    while (true)
    {
        const size_t count = detail::epoll_wait(m_epoll, { &event, 1 }, -1);
        if (count == 0)
        {
            continue;
        }

        if (event.data.fd == m_streamer.fd()) [[likely]]
        {
            receive();
        }
        else if (event.data.fd == m_timer)
        {
            [[maybe_unused]] const uint64_t ticks = detail::drain(m_timer);

            m_streamer.heartbeat();
            receive();
        }
        else if (event.data.fd == m_stop)
        {
            [[maybe_unused]] const uint64_t result = detail::drain(m_stop);
            break;
        }
    }

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
        else
        {
            orders();
        }
    }

    const bool connected = m_streamer.connected();
    if (m_connected && !connected) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx account stream lost: balance is stale until reconnect");
        emit(AccountEvent{ Balance{}, 0, false });
    }

    m_connected = connected;

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

    emit(AccountEvent{ balance, m_streamer.timestamp(), true });
}

template<typename Streamer>
void Account<Streamer>::positions() noexcept
{
    const std::span<const Position> positions = m_streamer.positions();

    m_log.log("okx account positions: count = {}", positions.size());

    for (const Position &position : positions)
    {
        m_log.log("okx account position: instrument = {}, side = {}, size = {}, price = {}, liquidation = {}, margin = {}, pnl = {}",
                  std::string_view(position.instrument), std::string_view(position.side), position.size, position.price,
                  position.liquidation, position.margin, position.pnl);
    }
}

template<typename Streamer>
void Account<Streamer>::orders() noexcept
{
    const std::span<const Order> orders = m_streamer.orders();

    m_log.log("okx account orders: count = {}", orders.size());

    for (const Order &order : orders)
    {
        m_log.log("okx account order: instrument = {}, id = {}, side = {}, position = {}, state = {}, size = {}, filled = {}, price = {}",
                  std::string_view(order.instrument), std::string_view(order.id), std::string_view(order.side), std::string_view(order.position),
                  std::string_view(order.state), order.size, order.filled, order.price);
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
