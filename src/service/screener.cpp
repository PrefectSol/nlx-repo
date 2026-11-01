#include "service/screener.h"
#include "core/fatal.h"
#include "core/string.h"
#include "core/time.h"

#include <algorithm>
#include <array>

namespace nlx::service
{

Screener::Screener(const OkxConfig &config, exchange::OkxRateLimiter &limiter, logging::LoggerRegistry &registry) :
    m_log(),
    m_epoll(),
    m_stop(),
    m_update(),
    m_timer(),
    m_deadline(),
    m_signal(),
    m_initialized(false),
    m_request(nullptr),
    m_streamer(config, limiter, m_log, m_epoll, &Screener::deliver, this),
    m_signals(),
    m_histories(),
    m_tracks(),
    m_touched(),
    m_minute(0),
    m_frozen(kNever),
    m_opened(0),
    m_compute(0),
    m_slowest(0),
    m_expected(0),
    m_arrived(0),
    m_received(0),
    m_detected(0),
    m_open(false)
{
    m_log = registry.create_node("screener");

    m_tracks.fill(Track{ kNever, false });

    m_epoll = FileDescriptor::make_epoll();
    m_stop = FileDescriptor::make_eventfd(true);
    m_update = FileDescriptor::make_eventfd(true);
    m_timer = FileDescriptor::make_timerfd(kTickInterval, true);
    m_deadline = FileDescriptor::make_timerfd(true);
    m_signal = FileDescriptor::make_eventfd(true);

    nlx::detail::epoll_add(m_epoll, m_stop);
    nlx::detail::epoll_add(m_epoll, m_update);
    nlx::detail::epoll_add(m_epoll, m_timer);
    nlx::detail::epoll_add(m_epoll, m_deadline);
}

void Screener::initialize()
{
    m_streamer.initialize();

    m_initialized.store(true, std::memory_order_release);
}

void Screener::request_stop() noexcept
{
    nlx::detail::notify(m_stop);
}

bool Screener::resubscribe(ScreenerRequest &request) noexcept
{
    if (request.instruments.size() > kInstrumentsCapacity) [[unlikely]]
    {
        return false;
    }

    ScreenerRequest *expected = nullptr;
    if (!m_request.compare_exchange_strong(expected, &request, std::memory_order_release, std::memory_order_relaxed)) [[unlikely]]
    {
        return false;
    }

    nlx::detail::notify(m_update);
    return true;
}

bool Screener::poll(ScreenerSignal &signal) noexcept
{
    return m_signals.pop(signal);
}

const FileDescriptor &Screener::signals() const noexcept
{
    return m_signal;
}

void Screener::start() noexcept
{
    if (!m_initialized.load(std::memory_order_acquire)) [[unlikely]]
    {
        NLX_FATAL("nlx::service::Screener::start() called before initialize()");
    }

    m_log.log("screener started: instruments capacity = {}, history = {}, warmup = {}, deadline = {} ms, cascade = {} instruments",
              kInstrumentsCapacity, kHistoryCapacity, kWarmupBars, kMinuteDeadline.count(), kCascadeThreshold);

    m_streamer.tick(time::now_monotonic());

    epoll_event event{};
    while (true)
    {
        const size_t count = nlx::detail::epoll_wait(m_epoll, { &event, 1 }, -1);
        if (count == 0)
        {
            continue;
        }

        if (event.data.fd == m_streamer.fd()) [[likely]]
        {
            m_streamer.receive();
        }
        else if (event.data.fd == m_deadline)
        {
            [[maybe_unused]] const uint64_t expirations = nlx::detail::drain(m_deadline);

            if (m_open)
            {
                complete("deadline");
            }
        }
        else if (event.data.fd == m_timer)
        {
            [[maybe_unused]] const uint64_t ticks = nlx::detail::drain(m_timer);

            if (!m_open)
            {
                m_streamer.tick(time::now_monotonic());
            }
        }
        else if (event.data.fd == m_update)
        {
            [[maybe_unused]] const uint64_t result = nlx::detail::drain(m_update);
            update();
        }
        else if (event.data.fd == m_stop)
        {
            [[maybe_unused]] const uint64_t result = nlx::detail::drain(m_stop);
            break;
        }
    }

    m_streamer.close();
    update();

    m_log.log("screener stopped");
}

void Screener::deliver(void *object, size_t index, const exchange::OkxBar &bar) noexcept
{
    static_cast<Screener *>(object)->accept(index, bar);
}

void Screener::update() noexcept
{
    ScreenerRequest *const request = m_request.load(std::memory_order_acquire);
    if (request == nullptr)
    {
        return;
    }

    size_t ignored = 0;
    const size_t touched = m_streamer.update(request->instruments, m_touched, ignored);

    for (size_t position = 0; position < touched; ++position)
    {
        reset(m_touched[position]);
    }

    if (m_open && m_expected != 0 && m_arrived == m_expected)
    {
        complete("all");
    }

    request->ignored = ignored;
    request->connected = m_streamer.connected();

    m_log.log("screener instruments updated: instruments = {}, reset = {}, ignored = {}, connected = {}",
              request->instruments.size(), touched, ignored, request->connected);

    m_request.store(nullptr, std::memory_order_release);
    nlx::detail::notify(request->notify);
}

void Screener::reset(size_t index) noexcept
{
    History &history = m_histories[index];

    if (m_open && !history.empty() && history.back().time == m_minute - kBarInterval)
    {
        --m_expected;
    }

    history.clear();
    m_tracks[index] = Track{ kNever, false };
}

void Screener::accept(size_t index, const exchange::OkxBar &bar) noexcept
{
    const std::string_view id = m_streamer.id(index);

    if (bar.time.count() % kBarInterval.count() != 0) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("screener {} misaligned close ignored: time = {}", id, bar.time.count());
        return;
    }

    if (bar.time > m_minute)
    {
        open(bar.time);
    }

    History &history = m_histories[index];

    bool continued = false;
    bool corrected = false;

    if (history.empty()) [[unlikely]]
    {
        history.push(bar);
    }
    else
    {
        const milliseconds last = history.back().time;
        const milliseconds difference = bar.time - last;

        if (difference == kBarInterval) [[likely]]
        {
            history.push(bar);
            continued = true;
        }
        else if (difference == milliseconds::zero())
        {
            history.replace(bar);
            corrected = true;
        }
        else if (difference > kBarInterval)
        {
            m_log.log<logging::LogLevel::Warning>("screener {} gap: last = {}, time = {}, history = {} reset", id, last.count(), bar.time.count(), history.size());

            history.clear();
            history.push(bar);
        }
        else
        {
            m_log.log<logging::LogLevel::Warning>("screener {} stale close ignored: last = {}, time = {}", id, last.count(), bar.time.count());
            return;
        }
    }

    if (bar.time != m_minute || !m_open) [[unlikely]]
    {
        if (continued || corrected)
        {
            m_log.log<logging::LogLevel::Warning>("screener {} late close: time = {}, minute = {}, corrected = {}", id, bar.time.count(), m_minute.count(), corrected);
        }

        return;
    }

    if (corrected) [[unlikely]]
    {
        evaluate(index);

        m_log.log<logging::LogLevel::Warning>("screener {} correction recomputed: time = {}, pending = {}", id, bar.time.count(), m_tracks[index].pending);
        return;
    }

    ++m_received;
    m_arrived += static_cast<size_t>(continued);

    evaluate(index);

    if (m_expected != 0 && m_arrived == m_expected)
    {
        complete("all");
    }
}

void Screener::open(milliseconds minute) noexcept
{
    if (m_open) [[unlikely]]
    {
        complete("superseded");
    }

    const milliseconds previous = minute - kBarInterval;

    m_expected = 0;
    for (const History &history : m_histories)
    {
        m_expected += static_cast<size_t>(!history.empty() && history.back().time == previous);
    }

    m_minute = minute;
    m_opened = time::now_monotonic();
    m_open = true;

    m_arrived = 0;
    m_received = 0;
    m_detected = 0;
    m_compute = nanoseconds::zero();
    m_slowest = nanoseconds::zero();

    nlx::detail::arm(m_deadline, kMinuteDeadline);
}

void Screener::evaluate(size_t index) noexcept
{
    Track &track = m_tracks[index];
    const History &history = m_histories[index];

    track.pending = false;

    if (history.size() < kWarmupBars || m_minute - track.triggered < kTriggerCooldown)
    {
        return;
    }

    const nanoseconds begin = time::now_monotonic();
    track.pending = detect(history.history());
    const nanoseconds elapsed = time::now_monotonic() - begin;

    m_compute += elapsed;
    m_slowest = std::max(m_slowest, elapsed);
    ++m_detected;
}

void Screener::complete(std::string_view reason) noexcept
{
    const nanoseconds begin = time::now_monotonic();

    m_open = false;

    size_t triggers = 0;
    size_t window = 0;

    for (const Track &track : m_tracks)
    {
        triggers += static_cast<size_t>(track.pending);
        window += static_cast<size_t>(!track.pending && m_minute - track.triggered < kCascadeWindow);
    }

    const size_t total = triggers + window;
    const bool cascade = total > kCascadeThreshold;
    const bool frozen = m_minute < m_frozen;

    bool emitted = false;

    if (cascade)
    {
        m_frozen = std::max(m_frozen, m_minute + kCascadeCooldown);

        if (!frozen)
        {
            emitted = emit(ScreenerEvent::Cascade, std::string_view(), total);

            m_log.log<logging::LogLevel::Warning>("screener cascade: time = {}, instruments = {}, frozen until = {}",
                                                  m_minute.count(), total, m_frozen.count());
        }
    }

    const bool suppressed = m_minute < m_frozen;

    for (size_t index = 0; index < kInstrumentsCapacity; ++index)
    {
        Track &track = m_tracks[index];
        if (!track.pending)
        {
            continue;
        }

        track.pending = false;
        track.triggered = m_minute;

        const std::string_view id = m_streamer.id(index);

        if (suppressed)
        {
            m_log.log<logging::LogLevel::Warning>("screener {} trigger suppressed: time = {}, frozen until = {}", id, m_minute.count(), m_frozen.count());
            continue;
        }

        emitted = emit(ScreenerEvent::Trigger, id, total) || emitted;

        m_log.log("screener {} trigger: time = {}, window = {}", id, m_minute.count(), total);
    }

    if (emitted)
    {
        nlx::detail::notify(m_signal);
    }

    const nanoseconds end = time::now_monotonic();
    const std::array<char, time::iso8601_ms_size> stamp = nlx::detail::format_iso8601_ms(m_minute);
    const std::string_view minute(stamp.data(), stamp.size() - 1);

    m_log.log("screener minute {} assembled: closes = {}/{}, received = {}, reason = {}, elapsed = {} ms",
              minute, m_arrived, m_expected, m_received, reason, nlx::detail::ms(end - m_opened));
    m_log.log("screener minute {} computed: detected = {}, compute = {} ms, slowest = {} ms, decide = {} ms, triggers = {}, window = {}, frozen = {}",
              minute, m_detected, nlx::detail::ms(m_compute), nlx::detail::ms(m_slowest), nlx::detail::ms(end - begin), triggers, total, suppressed);
}

bool Screener::emit(ScreenerEvent event, std::string_view id, size_t instruments) noexcept
{
    ScreenerSignal *const signal = m_signals.claim();
    if (signal == nullptr) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("screener signal dropped: id = {}, time = {}, event = {}", id, m_minute.count(), static_cast<int>(event));
        return false;
    }

    nlx::detail::str_assign(signal->id, id);
    signal->time = m_minute;
    signal->instruments = instruments;
    signal->event = event;

    [[maybe_unused]] const bool committed = m_signals.commit();
    return true;
}

bool Screener::detect(std::span<const exchange::OkxBar> bars) const noexcept
{
    return false;
}

} // namespace nlx::service
