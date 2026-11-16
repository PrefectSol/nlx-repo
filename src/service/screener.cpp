#include "service/screener.h"
#include "core/fatal.h"
#include "core/string.h"
#include "exchange/okx_candle_streamer.h"

#include <algorithm>

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
    m_signal(),
    m_initialized(false),
    m_request(nullptr),
    m_cooldown(0),
    m_signals(),
    m_rings(),
    m_generation(0),
    m_ticks(0),
    m_gaps(0),
    m_gap_index(0),
    m_gap_candles(0),
    m_stale(0)
{
    m_log = registry.create_node("okx_screener");

    m_epoll = FileDescriptor::make_epoll();
    m_stop = FileDescriptor::make_eventfd(true);
    m_update = FileDescriptor::make_eventfd(true);
    m_done = FileDescriptor::make_eventfd(true);
    m_timer = FileDescriptor::make_timerfd(milliseconds(kTimerMilliseconds), true);
    m_signal = FileDescriptor::make_eventfd(true);

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
    m_log.log("filter candles: window = {}, baseline = {}, cooldown = {}", kWindowCandles, kBaselineCandles, kCooldownCandles);
    m_log.log("filter impulse: threshold = {}, ratio = {}", kImpulseThreshold, kImpulseRatio);
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
            receive();
        }
        else if (event.data.fd == m_timer)
        {
            [[maybe_unused]] const uint64_t ticks = detail::drain(m_timer);
            tick();
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

    reset();
    m_log.log<logging::LogLevel::Warning>("screener history reset: resubscribed, instruments = {}, blind for {} candles", m_streamer.size(), kCooldownCandles);

    m_request.store(nullptr, std::memory_order_release);
    detail::notify(m_done);
}

template<typename Streamer>
void Screener<Streamer>::cooldown() noexcept
{
    m_cooldown.fetch_add(1, std::memory_order_release);
}

template<typename Streamer>
const FileDescriptor &Screener<Streamer>::signals() const noexcept
{
    return m_signal;
}

template<typename Streamer>
bool Screener<Streamer>::pop(ScreenerSignal &signal) noexcept
{
    const uint64_t generation = m_cooldown.load(std::memory_order_relaxed);

    while (m_signals.pop(signal))
    {
        if (signal.generation == generation) [[likely]]
        {
            return true;
        }
    }

    return false;
}

template<typename Streamer>
size_t Screener<Streamer>::discard() noexcept
{
    [[maybe_unused]] const uint64_t result = detail::drain(m_signal);

    size_t count = 0;

    ScreenerSignal signal{};
    while (m_signals.pop(signal))
    {
        ++count;
    }

    return count;
}

template<typename Streamer>
void Screener<Streamer>::tick() noexcept
{
    m_streamer.heartbeat();
    synchronize();

    if (m_gaps != 0) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("screener history reset: gaps, instruments = {}, missed candles max = {}, example = {}, blind for {} candles",
                                              m_gaps, m_gap_candles, m_streamer.instrument(m_gap_index), kCooldownCandles);
        m_gaps = 0;
        m_gap_candles = 0;
    }

    if (m_stale != 0) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("screener candles ignored: stale = {}", m_stale);
        m_stale = 0;
    }

    if (++m_ticks < kStatusTicks)
    {
        return;
    }

    m_ticks = 0;

    const size_t size = m_streamer.size();
    const auto full = [](const Ring &ring) noexcept { return ring.full(); };
    const size_t ready = static_cast<size_t>(std::count_if(m_rings.begin(), m_rings.begin() + static_cast<std::ptrdiff_t>(size), full));

    m_log.log("screener status: instruments = {}, ready = {}, blind = {}", size, ready, size - ready);
}

template<typename Streamer>
void Screener<Streamer>::receive() noexcept
{
    size_t index = 0;
    while (m_streamer.poll(index))
    {
        synchronize();

        if (m_streamer.extract(m_rings[index].claim())) [[likely]]
        {
            accept(index);
        }
    }
}

template<typename Streamer>
void Screener<Streamer>::accept(size_t index) noexcept
{
    Ring &ring = m_rings[index];
    const exchange::OkxCandle &candle = ring.claim();

    if (!(candle.close > 0.0)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("screener candle rejected: instrument = {}, timestamp = {}, close = {}",
                                            m_streamer.instrument(index), candle.timestamp, candle.close);
        return;
    }

    if (ring.size() != 0)
    {
        const int64_t expected = ring.back().timestamp + kCandleMilliseconds;

        if (candle.timestamp < expected) [[unlikely]]
        {
            ++m_stale;
            return;
        }

        if (candle.timestamp > expected) [[unlikely]]
        {
            const exchange::OkxCandle copy = candle;

            ++m_gaps;
            m_gap_index = index;
            m_gap_candles = std::max(m_gap_candles, (copy.timestamp - expected) / kCandleMilliseconds);

            ring.clear();
            ring.claim() = copy;
        }
    }

    ring.commit();

    if (!ring.full())
    {
        return;
    }

    double impulse = 0.0;
    double baseline = 0.0;

    if (!filter(ring.history().template last<kFilterCandles>(), impulse, baseline)) [[likely]]
    {
        return;
    }

    notify(index, ring.back(), impulse, baseline);
    ring.clear();
}

template<typename Streamer>
void Screener<Streamer>::notify(size_t index, const exchange::OkxCandle &candle, double impulse, double baseline) noexcept
{
    const std::string_view instrument = m_streamer.instrument(index);

    m_log.log("screener signal: instrument = {}, timestamp = {}, impulse = {}, baseline = {}, generation = {}",
              instrument, candle.timestamp, impulse, baseline, m_generation);

    ScreenerSignal *const signal = m_signals.claim();
    if (signal == nullptr) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("screener signal dropped: queue is full, instrument = {}", instrument);
        return;
    }

    nlx::detail::str_assign(signal->instrument, instrument);
    signal->timestamp = candle.timestamp;
    signal->impulse = impulse;
    signal->baseline = baseline;
    signal->generation = m_generation;

    [[maybe_unused]] const bool committed = m_signals.commit();

    detail::notify(m_signal);
}

template<typename Streamer>
void Screener<Streamer>::synchronize() noexcept
{
    const uint64_t requested = m_cooldown.load(std::memory_order_acquire);
    if (requested == m_generation) [[likely]]
    {
        return;
    }

    m_generation = requested;
    reset();

    m_log.log<logging::LogLevel::Warning>("screener history reset: cooldown, generation = {}, blind for {} candles", requested, kCooldownCandles);
}

template<typename Streamer>
void Screener<Streamer>::reset() noexcept
{
    for (Ring &ring : m_rings)
    {
        ring.clear();
    }
}

template class Screener<exchange::OkxCandleStreamer>;

} // namespace nlx::service
