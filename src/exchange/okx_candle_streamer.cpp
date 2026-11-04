#include "exchange/okx_candle_streamer.h"
#include "core/crypto.h"
#include "core/string.h"
#include "core/time.h"
#include "parsing/json_reader.h"

#include <format>

namespace nlx::exchange
{

OkxCandleStreamer::OkxCandleStreamer(OkxRateLimiter &limiter, logging::LoggerNode &log) :
    m_log(log),
    m_limiter(limiter),
    m_epoll(),
    m_client(),
    m_slots(),
    m_index(),
    m_size(0),
    m_subscribed(0),
    m_received(0),
    m_pinged(false)
{
    m_epoll = FileDescriptor::make_epoll();
}

void OkxCandleStreamer::initialize()
{
    m_client.initialize();
}

bool OkxCandleStreamer::resubscribe(std::span<const OkxInstrument> instruments) noexcept
{
    m_size = 0;
    m_index.fill(0);

    for (const OkxInstrument &instrument : instruments)
    {
        if (!instrument.accepted)
        {
            continue;
        }

        if (m_size == kInstrumentsCapacity) [[unlikely]]
        {
            break;
        }

        nlx::detail::str_assign(m_slots[m_size].id, instrument.id);
        insert(m_size);

        ++m_size;
    }

    return subscribe();
}

void OkxCandleStreamer::receive() noexcept
{
    do
    {
        std::string_view text;
        if (!read(text)) [[unlikely]]
        {
            return;
        }

        [[maybe_unused]] const bool response = dispatch(text);
    }
    while (m_client.pending(milliseconds::zero()));
}

void OkxCandleStreamer::heartbeat() noexcept
{
    if (m_size == 0)
    {
        return;
    }

    if (!m_client.connected())
    {
        [[maybe_unused]] const bool subscribed = subscribe();
        return;
    }

    const nanoseconds interval = milliseconds(kHeartbeatMilliseconds);
    const nanoseconds idle = time::now_monotonic() - m_received;

    if (idle >= 2 * interval) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx candle streamer heartbeat lost: idle = {} ms", nlx::detail::ms(idle));

        [[maybe_unused]] const bool subscribed = subscribe();
        return;
    }

    if (idle >= interval && !m_pinged)
    {
        const std::span<char> output = m_client.output();
        const auto ping = std::format_to_n(output.data(), output.size(), "ping");

        m_pinged = send(static_cast<size_t>(ping.size));
    }
}

void OkxCandleStreamer::close() noexcept
{
    m_client.close();
}

const FileDescriptor &OkxCandleStreamer::fd() const noexcept
{
    return m_epoll;
}

bool OkxCandleStreamer::subscribe() noexcept
{
    close();

    m_subscribed = 0;
    if (m_size == 0)
    {
        return false;
    }

    const nanoseconds begin = time::now_monotonic();
    if (!connect())
    {
        return false;
    }

    size_t sent = 0;
    while (sent < m_size)
    {
        const std::span<char> output = m_client.output();

        char *const data = output.data();
        char *const end = data + output.size() - 2;

        char *out = std::format_to_n(data, end - data, R"({{"op":"subscribe","args":[)").out;

        size_t index = sent;
        for (; index < m_size; ++index)
        {
            Slot &slot = m_slots[index];
            slot.subscribed = false;

            const std::string_view separator = index != sent ? "," : "";
            const auto argument = std::format_to_n(out, end - out, R"({}{{"channel":"candle1m","instId":"{}"}})", separator, std::string_view(slot.id));
            if (argument.size > end - out)
            {
                break;
            }

            out = argument.out;
        }

        *out++ = ']';
        *out++ = '}';

        acquire(WebsocketBusinessSubscribe);

        if (!send(static_cast<size_t>(out - data)))
        {
            return false;
        }

        sent = index;
    }

    size_t responses = 0;
    nanoseconds last = time::now_monotonic();

    while (responses < m_size)
    {
        const nanoseconds remaining = milliseconds(kResponseMilliseconds) - (time::now_monotonic() - last);
        if (remaining <= nanoseconds::zero() || !m_client.pending(nlx::detail::ceil_milliseconds(remaining)))
        {
            break;
        }

        std::string_view text;
        if (!read(text))
        {
            return false;
        }

        if (dispatch(text))
        {
            ++responses;
            last = time::now_monotonic();
        }
    }

    const nanoseconds end = time::now_monotonic();

    m_log.log("okx candle streamer subscribed: instruments = {}, subscribed = {}, elapsed = {} ms",
              m_size, m_subscribed, nlx::detail::ms(end - begin));

    return m_subscribed != 0;
}

bool OkxCandleStreamer::connect() noexcept
{
    acquire(WebsocketConnect);

    const nanoseconds begin = time::now_monotonic();
    if (!m_client.connect(kUrl, milliseconds(kConnectMilliseconds)))
    {
        m_log.log<logging::LogLevel::Error>("okx candle streamer connect failed: {}", m_client.error());
        return false;
    }

    if (!nlx::detail::epoll_watch(m_epoll, m_client.fd())) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx candle streamer epoll registration failed");
        m_client.close();

        return false;
    }

    m_received = time::now_monotonic();
    m_pinged = false;

    m_log.log("okx candle streamer connected in {} ms", nlx::detail::ms(m_received - begin));

    return true;
}

void OkxCandleStreamer::acquire(OkxRequest request) noexcept
{
    nanoseconds delay = m_limiter.acquire(request, time::now_monotonic());
    while (delay != nanoseconds::zero())
    {
        m_log.log<logging::LogLevel::Warning>("okx candle streamer throttled for {} ms", nlx::detail::ms(delay));

        time::sleep(delay);
        delay = m_limiter.acquire(request, time::now_monotonic());
    }
}

bool OkxCandleStreamer::send(size_t size) noexcept
{
    if (m_client.send(size)) [[likely]]
    {
        return true;
    }

    m_log.log<logging::LogLevel::Error>("okx candle streamer send failed: {}", m_client.error());
    close();

    return false;
}

bool OkxCandleStreamer::read(std::string_view &text) noexcept
{
    const network::WebsocketStatus status = m_client.read(text, milliseconds(kResponseMilliseconds));

    if (status == network::WebsocketStatus::Closed) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx candle streamer closed by server: {}", m_client.error());
        close();

        return false;
    }

    if (status == network::WebsocketStatus::Failed) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx candle streamer read failed: {}", m_client.error());
        close();

        return false;
    }

    m_received = time::now_monotonic();
    m_pinged = false;

    return true;
}

bool OkxCandleStreamer::dispatch(std::string_view text) noexcept
{
    if (!text.starts_with(R"({"event":)")) [[likely]]
    {
        return false;
    }

    std::string_view event;
    std::string_view instrument;
    int code = 0;
    std::string_view message;

    if (!parsing::detail::okx_parse_event(text, event, instrument, code, message)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx candle streamer event parse failed: {}", text);
        return false;
    }

    if (event == "subscribe")
    {
        const size_t index = find(instrument);
        if (index != m_size && !m_slots[index].subscribed)
        {
            m_slots[index].subscribed = true;
            ++m_subscribed;
        }

        return true;
    }

    if (event == "error")
    {
        m_log.log<logging::LogLevel::Error>("okx candle streamer error: code = {}, message = {}", code, message);
        return true;
    }

    m_log.log<logging::LogLevel::Warning>("okx candle streamer event: {}", text);

    return false;
}

size_t OkxCandleStreamer::find(std::string_view id) const noexcept
{
    const size_t mask = m_index.size() - 1;

    for (size_t position = crypto::fnv1a(id) & mask; m_index[position] != 0; position = (position + 1) & mask)
    {
        const size_t index = m_index[position] - 1u;
        if (std::string_view(m_slots[index].id) == id)
        {
            return index;
        }
    }

    return m_size;
}

void OkxCandleStreamer::insert(size_t index) noexcept
{
    const size_t mask = m_index.size() - 1;

    size_t position = crypto::fnv1a(m_slots[index].id) & mask;
    while (m_index[position] != 0)
    {
        position = (position + 1) & mask;
    }

    m_index[position] = static_cast<uint16_t>(index + 1);
}

} // namespace nlx::exchange
