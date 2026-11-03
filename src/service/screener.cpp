#include "service/screener.h"
#include "core/fatal.h"
#include "core/time.h"
#include "exchange/okx_candle_streamer.h"

namespace nlx::service
{

template<typename Streamer>
Screener<Streamer>::Screener(Streamer &streamer, logging::LoggerRegistry &registry, std::string_view name) :
    m_log(),
    m_streamer(streamer),
    m_epoll(),
    m_stop(),
    m_update(),
    m_timer(),
    m_initialized(false),
    m_request(nullptr)
{
    m_log = registry.create_node(name);

    m_epoll = FileDescriptor::make_epoll();
    m_stop = FileDescriptor::make_eventfd(true);
    m_update = FileDescriptor::make_eventfd(true);
    m_timer = FileDescriptor::make_timerfd(kTickInterval, true);

    detail::epoll_add(m_epoll, m_stop);
    detail::epoll_add(m_epoll, m_update);
    detail::epoll_add(m_epoll, m_timer);
}

template<typename Streamer>
void Screener<Streamer>::initialize()
{
    m_streamer.initialize(m_epoll);

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
    m_log.log("screener instruments capacity: {}", Streamer::kInstrumentsCapacity);
    m_log.flush();

    m_streamer.tick(time::now_monotonic());

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
            m_streamer.tick(time::now_monotonic());
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
    update();

    m_log.log("screener stopped");
    m_log.flush();
}

template<typename Streamer>
void Screener<Streamer>::request_stop() noexcept
{
    detail::notify(m_stop);
}

template<typename Streamer>
bool Screener<Streamer>::resubscribe(ScreenerRequest &request) noexcept
{
    if (request.instruments.size() > Streamer::kInstrumentsCapacity) [[unlikely]]
    {
        return false;
    }

    ScreenerRequest *expected = nullptr;
    if (!m_request.compare_exchange_strong(expected, &request, std::memory_order_release, std::memory_order_relaxed)) [[unlikely]]
    {
        return false;
    }

    detail::notify(m_update);
    return true;
}

template<typename Streamer>
void Screener<Streamer>::update() noexcept
{
    ScreenerRequest *const request = m_request.load(std::memory_order_acquire);
    if (request == nullptr)
    {
        return;
    }

    const nanoseconds begin = time::now_monotonic();
    request->accepted = m_streamer.resubscribe(request->instruments);
    const nanoseconds end = time::now_monotonic();

    m_log.log("screener instruments resubscribed: instruments = {}, accepted = {}, connected = {}, elapsed = {} ms",
              request->instruments.size(), request->accepted, m_streamer.connected(), detail::ms(end - begin));

    m_request.store(nullptr, std::memory_order_release);
    detail::notify(request->notify);
}

template class Screener<exchange::OkxCandleStreamer>;

} // namespace nlx::service
