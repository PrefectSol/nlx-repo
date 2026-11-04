#include "service/screener.h"
#include "core/fatal.h"
#include "exchange/okx_candle_streamer.h"

namespace nlx::service
{

template<typename Streamer>
Screener<Streamer>::Screener(exchange::OkxRateLimiter &limiter, logging::LoggerRegistry &registry) :
    m_log(),
    m_streamer(limiter, m_log),
    m_epoll(),
    m_stop(),
    m_update(),
    m_done(),
    m_timer(),
    m_initialized(false),
    m_request(nullptr)
{
    m_log = registry.create_node("okx_screener");

    m_epoll = FileDescriptor::make_epoll();
    m_stop = FileDescriptor::make_eventfd(true);
    m_update = FileDescriptor::make_eventfd(true);
    m_done = FileDescriptor::make_eventfd(true);
    m_timer = FileDescriptor::make_timerfd(milliseconds(kTimerMilliseconds), true);

    detail::epoll_add(m_epoll, m_stop);
    detail::epoll_add(m_epoll, m_update);
    detail::epoll_add(m_epoll, m_timer);
    detail::epoll_add(m_epoll, m_streamer.fd());
}

template<typename Streamer>
void Screener<Streamer>::initialize()
{
    m_streamer.initialize();

    m_initialized.store(true, std::memory_order_release);
}

template<typename Streamer>
void Screener<Streamer>::start() noexcept
{
    if (!m_initialized.load(std::memory_order_acquire)) [[unlikely]]
    {
        NLX_FATAL("nlx::service::Screener::start() called before initialize()");
    }

    m_log.log("screener started");
    m_log.flush();

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
            m_streamer.receive();
        }
        else if (event.data.fd == m_timer)
        {
            [[maybe_unused]] const uint64_t ticks = detail::drain(m_timer);
            m_streamer.heartbeat();
        }
        else if (event.data.fd == m_update)
        {
            [[maybe_unused]] const uint64_t result = detail::drain(m_update);
            update();
        }
        else if (event.data.fd == m_stop)
        {
            [[maybe_unused]] const uint64_t result = detail::drain(m_stop);
            break;
        }
    }

    m_streamer.close();

    m_log.log("screener stopped");
    m_log.flush();
}

template<typename Streamer>
void Screener<Streamer>::request_stop() noexcept
{
    detail::notify(m_stop);
}

template<typename Streamer>
bool Screener<Streamer>::resubscribe(std::span<const exchange::OkxInstrument> instruments) noexcept
{
    Request request{ instruments, false };

    Request *expected = nullptr;
    if (!m_request.compare_exchange_strong(expected, &request, std::memory_order_release, std::memory_order_relaxed)) [[unlikely]]
    {
        return false;
    }

    detail::notify(m_update);
    detail::wait(m_done);

    return request.result;
}

template<typename Streamer>
void Screener<Streamer>::update() noexcept
{
    Request *const request = m_request.load(std::memory_order_acquire);
    if (request == nullptr)
    {
        return;
    }

    request->result = m_streamer.resubscribe(request->instruments);

    m_request.store(nullptr, std::memory_order_release);
    detail::notify(m_done);
}

template class Screener<exchange::OkxCandleStreamer>;

} // namespace nlx::service
