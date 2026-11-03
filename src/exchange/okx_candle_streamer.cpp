#include "exchange/okx_candle_streamer.h"
#include "core/fatal.h"
#include "core/string.h"
#include "core/time.h"

#include <algorithm>
#include <format>

namespace nlx::exchange
{

namespace
{

constexpr std::string_view event_prefix = R"({"event":")";
constexpr std::string_view subscribe_event = R"({"event":"subscribe")";
constexpr std::string_view unsubscribe_event = R"({"event":"unsubscribe")";
constexpr std::string_view error_event = R"({"event":"error")";
constexpr std::string_view request_suffix = "]}";
constexpr std::string_view ping_message = "ping";
constexpr std::string_view pong_message = "pong";

constexpr bool valid(std::string_view id) noexcept
{
    if (id.empty() || id.size() >= OkxInstrument::kIdCapacity)
    {
        return false;
    }

    return std::ranges::none_of(id, [](char ch) { return ch == '"' || ch == '\\' || static_cast<unsigned char>(ch) < 0x20; });
}

} // namespace

OkxCandleStreamer::OkxCandleStreamer(OkxRateLimiter &limiter, logging::LoggerRegistry &registry) :
    m_log(),
    m_limiter(limiter),
    m_epoll(nullptr),
    m_client(),
    m_slots(),
    m_opened(0),
    m_received(0),
    m_pinged(0),
    m_retry(0),
    m_backoff(kReconnectDelay),
    m_resume(0),
    m_reported(0),
    m_messages(0),
    m_bytes(0),
    m_acks(0),
    m_errors(0)
{
    m_log = registry.create_node("okx_candles");
}

void OkxCandleStreamer::initialize(const FileDescriptor &epoll)
{
    m_client.initialize();
    m_epoll = &epoll;
}

void OkxCandleStreamer::tick(nanoseconds now) noexcept
{
    if (m_epoll == nullptr) [[unlikely]]
    {
        NLX_FATAL("nlx::exchange::OkxCandleStreamer::tick() called before initialize()");
    }

    if (!m_client.connected())
    {
        if (now >= m_retry)
        {
            [[maybe_unused]] const bool connected = connect(now);
        }
    }
    else
    {
        keepalive(now);
        subscribe();
    }

    report(now);
}

void OkxCandleStreamer::receive() noexcept
{
    do
    {
        std::string_view text;
        const network::WebsocketStatus status = m_client.read(text, kReadTimeout);

        if (status == network::WebsocketStatus::Closed) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Warning>("okx candle streamer closed by server: {}", m_client.error());
            disconnect();

            return;
        }

        if (status == network::WebsocketStatus::Failed) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Error>("okx candle streamer read failed: {}", m_client.error());
            disconnect();

            return;
        }

        m_received = time::now_monotonic();
        m_pinged = nanoseconds::zero();

        if (status == network::WebsocketStatus::Message) [[likely]]
        {
            dispatch(text);
        }
    }
    while (m_client.pending());
}

void OkxCandleStreamer::close() noexcept
{
    if (!m_client.connected())
    {
        return;
    }

    nlx::detail::epoll_unwatch(*m_epoll, m_client.fd());
    m_client.close();
}

size_t OkxCandleStreamer::resubscribe(std::span<const std::string_view> instruments) noexcept
{
    for (Slot &slot : m_slots)
    {
        slot.wanted = false;
    }

    for (const std::string_view id : instruments)
    {
        const size_t index = find(id);
        if (index != kInstrumentsCapacity)
        {
            m_slots[index].wanted = true;
        }
    }

    if (m_client.connected() && !submit(false)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx candle streamer unsubscribe skipped");
    }

    size_t removed = 0;
    for (Slot &slot : m_slots)
    {
        if (slot.size != 0 && !slot.wanted)
        {
            slot = Slot{};
            ++removed;
        }
    }

    size_t added = 0;
    size_t ignored = 0;
    size_t vacant = 0;

    for (const std::string_view id : instruments)
    {
        if (!valid(id)) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Warning>("okx candle streamer instrument ignored: id = {}", id);
            ++ignored;

            continue;
        }

        if (find(id) != kInstrumentsCapacity)
        {
            continue;
        }

        while (vacant < kInstrumentsCapacity && m_slots[vacant].size != 0)
        {
            ++vacant;
        }

        if (vacant == kInstrumentsCapacity) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Warning>("okx candle streamer instrument ignored: id = {}, capacity = {}", id, kInstrumentsCapacity);
            ++ignored;

            continue;
        }

        Slot &slot = m_slots[vacant];
        nlx::detail::str_assign(slot.id, id);
        slot.size = static_cast<uint8_t>(id.size());
        slot.wanted = true;
        slot.pending = true;

        ++added;
    }

    subscribe();

    const size_t accepted = count();

    m_log.log("okx candle streamer resubscribed: instruments = {}, accepted = {}, added = {}, removed = {}, ignored = {}, connected = {}",
              instruments.size(), accepted, added, removed, ignored, m_client.connected());

    return accepted;
}

bool OkxCandleStreamer::connected() const noexcept
{
    return m_client.connected();
}

int OkxCandleStreamer::fd() const noexcept
{
    return m_client.fd();
}

bool OkxCandleStreamer::connect(nanoseconds now) noexcept
{
    const nanoseconds delay = m_limiter.acquire(WebsocketConnect, now);
    if (delay != nanoseconds::zero()) [[unlikely]]
    {
        m_retry = now + delay;
        m_log.log<logging::LogLevel::Warning>("okx candle streamer connect throttled for {} ms", nlx::detail::ms(delay));

        return false;
    }

    m_log.log("okx candle streamer connecting to {}", kUrl);

    if (!m_client.connect(kUrl, kConnectTimeout))
    {
        const nanoseconds end = time::now_monotonic();
        schedule(end);

        m_log.log<logging::LogLevel::Error>("okx candle streamer connect failed in {} ms: {}, reconnect in {} ms",
                                            nlx::detail::ms(end - now), m_client.error(), nlx::detail::ms(m_retry - end));

        return false;
    }

    if (!nlx::detail::epoll_watch(*m_epoll, m_client.fd())) [[unlikely]]
    {
        m_client.close();
        schedule(time::now_monotonic());

        m_log.log<logging::LogLevel::Error>("okx candle streamer epoll registration failed");

        return false;
    }

    m_limiter.reset(WebsocketBusinessSubscribe);

    for (Slot &slot : m_slots)
    {
        slot.pending = slot.size != 0;
    }

    const nanoseconds end = time::now_monotonic();

    m_opened = end;
    m_received = end;
    m_pinged = nanoseconds::zero();
    m_resume = nanoseconds::zero();

    m_log.log("okx candle streamer connected in {} ms", nlx::detail::ms(end - now));

    subscribe();

    return true;
}

void OkxCandleStreamer::disconnect() noexcept
{
    close();

    const nanoseconds now = time::now_monotonic();
    if (now - m_opened >= kStableInterval)
    {
        m_backoff = kReconnectDelay;
    }

    schedule(now);

    m_log.log<logging::LogLevel::Warning>("okx candle streamer disconnected after {} ms, reconnect in {} ms",
                                          nlx::detail::ms(now - m_opened), nlx::detail::ms(m_retry - now));
}

void OkxCandleStreamer::schedule(nanoseconds now) noexcept
{
    m_retry = now + m_backoff;
    m_backoff = std::min(m_backoff * 2, kReconnectLimit);
}

void OkxCandleStreamer::keepalive(nanoseconds now) noexcept
{
    if (m_pinged != nanoseconds::zero())
    {
        if (now - m_pinged >= kPongTimeout) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Error>("okx candle streamer pong timeout");
            disconnect();
        }

        return;
    }

    if (now - m_received < kPingInterval)
    {
        return;
    }

    std::ranges::copy(ping_message, m_client.output().begin());

    if (send(ping_message.size()))
    {
        m_pinged = now;
    }
}

void OkxCandleStreamer::subscribe() noexcept
{
    if (!m_client.connected() || time::now_monotonic() < m_resume)
    {
        return;
    }

    [[maybe_unused]] const bool submitted = submit(true);
}

bool OkxCandleStreamer::submit(bool subscribing) noexcept
{
    const std::string_view operation = subscribing ? "subscribe" : "unsubscribe";
    const auto matches = [subscribing](const Slot &slot) { return slot.size != 0 && (subscribing ? slot.pending : !slot.wanted); };

    size_t begin = 0;
    while (begin < kInstrumentsCapacity)
    {
        const std::span<char> output = m_client.output();

        char *const data = output.data();
        char *const end = data + output.size() - request_suffix.size();

        const auto head = std::format_to_n(data, end - data, R"({{"op":"{}","args":[)", operation);
        char *out = head.out;

        size_t channels = 0;
        size_t index = begin;
        for (; index < kInstrumentsCapacity; ++index)
        {
            const Slot &slot = m_slots[index];
            if (!matches(slot))
            {
                continue;
            }

            const std::string_view separator = channels != 0 ? "," : "";
            const auto argument = std::format_to_n(out, end - out, R"({}{{"channel":"candle1m","instId":"{}"}})", separator, name(slot));
            if (argument.size > end - out)
            {
                break;
            }

            out = argument.out;
            ++channels;
        }

        if (channels == 0)
        {
            return true;
        }

        out = std::ranges::copy(request_suffix, out).out;

        const nanoseconds now = time::now_monotonic();
        const nanoseconds delay = m_limiter.acquire(WebsocketBusinessSubscribe, now);
        if (delay != nanoseconds::zero()) [[unlikely]]
        {
            m_resume = now + delay;
            m_log.log<logging::LogLevel::Warning>("okx candle streamer {} throttled for {} ms", operation, nlx::detail::ms(delay));

            return false;
        }

        const size_t size = static_cast<size_t>(out - data);
        if (!send(size))
        {
            return false;
        }

        if (subscribing)
        {
            for (size_t position = begin; position < index; ++position)
            {
                m_slots[position].pending = false;
            }
        }

        m_log.log("okx candle streamer {} sent: channels = {}, bytes = {}", operation, channels, size);

        begin = index;
    }

    return true;
}

bool OkxCandleStreamer::send(size_t size) noexcept
{
    if (m_client.send(size)) [[likely]]
    {
        return true;
    }

    m_log.log<logging::LogLevel::Error>("okx candle streamer send failed: {}", m_client.error());
    disconnect();

    return false;
}

void OkxCandleStreamer::dispatch(std::string_view text) noexcept
{
    if (!text.starts_with(event_prefix)) [[likely]]
    {
        if (text != pong_message)
        {
            ++m_messages;
            m_bytes += text.size();
        }

        return;
    }

    if (text.starts_with(subscribe_event) || text.starts_with(unsubscribe_event)) [[likely]]
    {
        ++m_acks;
        return;
    }

    if (text.starts_with(error_event))
    {
        ++m_errors;
        m_log.log<logging::LogLevel::Error>("okx candle streamer error: {}", text);

        return;
    }

    m_log.log<logging::LogLevel::Warning>("okx candle streamer event: {}", text);
}

void OkxCandleStreamer::report(nanoseconds now) noexcept
{
    if (now - m_reported < kReportInterval)
    {
        return;
    }

    m_log.log("okx candle streamer report: connected = {}, instruments = {}, messages = {}, bytes = {}, acks = {}, errors = {}",
              m_client.connected(), count(), m_messages, m_bytes, m_acks, m_errors);

    m_reported = now;
    m_messages = 0;
    m_bytes = 0;
    m_acks = 0;
    m_errors = 0;
}

size_t OkxCandleStreamer::find(std::string_view id) const noexcept
{
    for (size_t index = 0; index < kInstrumentsCapacity; ++index)
    {
        const Slot &slot = m_slots[index];
        if (slot.size != 0 && name(slot) == id)
        {
            return index;
        }
    }

    return kInstrumentsCapacity;
}

size_t OkxCandleStreamer::count() const noexcept
{
    return static_cast<size_t>(std::ranges::count_if(m_slots, [](const Slot &slot) { return slot.size != 0; }));
}

std::string_view OkxCandleStreamer::name(const Slot &slot) noexcept
{
    return std::string_view(slot.id, slot.size);
}

} // namespace nlx::exchange
