#include "exchange/okx_streamer.h"
#include "core/string.h"
#include "core/time.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <stdexcept>

namespace nlx::exchange
{

namespace
{

constexpr std::string_view candle_prefix = R"({"arg":{"channel":"candle1m","instId":")";
constexpr std::string_view data_prefix = R"("},"data":[[")";
constexpr std::string_view field_separator = R"(",")";
constexpr std::string_view forming_suffix = R"(,"0"]]})";
constexpr std::string_view closed_suffix = R"(1"]]})";
constexpr std::string_view event_prefix = R"({"event":")";
constexpr std::string_view request_suffix = "]}";
constexpr std::string_view ping_message = "ping";
constexpr std::string_view pong_message = "pong";

constexpr bool json_safe(std::string_view value) noexcept
{
    return std::ranges::none_of(value, [](char ch) { return ch == '"' || ch == '\\' || static_cast<unsigned char>(ch) < 0x20; });
}

constexpr size_t hash(std::string_view value) noexcept
{
    uint64_t result = 14695981039346656037ULL;
    for (const char ch : value)
    {
        result ^= static_cast<unsigned char>(ch);
        result *= 1099511628211ULL;
    }

    return static_cast<size_t>(result);
}

template<typename T>
bool parse_field(std::string_view &text, T &value) noexcept
{
    const char *const end = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(text.data(), end, value);

    const std::string_view rest(result.ptr, end);
    if (result.ec != std::errc() || !rest.starts_with(field_separator)) [[unlikely]]
    {
        return false;
    }

    text = rest.substr(field_separator.size());
    return true;
}

bool parse_candle(std::string_view text, std::string_view &id, OkxBar &bar) noexcept
{
    text.remove_prefix(candle_prefix.size());

    const size_t quote = text.substr(0, OkxInstrument::kIdCapacity).find('"');
    if (quote == std::string_view::npos || quote == 0) [[unlikely]]
    {
        return false;
    }

    id = text.substr(0, quote);
    text.remove_prefix(quote);

    if (!text.starts_with(data_prefix)) [[unlikely]]
    {
        return false;
    }

    text.remove_prefix(data_prefix.size());

    int64_t time = 0;

    bool parsed = parse_field(text, time);
    parsed = parsed && parse_field(text, bar.open);
    parsed = parsed && parse_field(text, bar.high);
    parsed = parsed && parse_field(text, bar.low);
    parsed = parsed && parse_field(text, bar.close);
    parsed = parsed && parse_field(text, bar.volume);
    parsed = parsed && parse_field(text, bar.currency_volume);
    parsed = parsed && parse_field(text, bar.quote_volume);

    if (!parsed || text != closed_suffix) [[unlikely]]
    {
        return false;
    }

    bar.time = milliseconds(time);
    return true;
}

} // namespace

OkxStreamer::OkxStreamer(const OkxConfig &config, OkxRateLimiter &limiter, logging::LoggerNode &log,
                         const FileDescriptor &epoll, CloseHandler handler, void *object) :
    m_log(log),
    m_limiter(limiter),
    m_epoll(epoll),
    m_handler(handler),
    m_object(object),
    m_client(),
    m_url(),
    m_opened(0),
    m_received(0),
    m_pinged(0),
    m_retry(0),
    m_backoff(kReconnectDelay),
    m_resume(0),
    m_reported(0),
    m_dirty(false),
    m_slots(),
    m_index(),
    m_messages(0),
    m_updates(0),
    m_closes(0),
    m_acks(0),
    m_errors(0),
    m_unknown(0),
    m_event()
{
    if (m_handler == nullptr) [[unlikely]]
    {
        throw std::invalid_argument("okx streamer close handler is invalid");
    }

    const std::string_view url(config.websocket_url, ::strnlen(config.websocket_url, sizeof(config.websocket_url)));
    if (url.empty()) [[unlikely]]
    {
        throw std::invalid_argument("okx websocket url is invalid");
    }

    const auto address = std::format_to_n(m_url, kUrlCapacity - 1, "{}{}", url, kPath);
    if (static_cast<size_t>(address.size) >= kUrlCapacity) [[unlikely]]
    {
        throw std::invalid_argument("okx websocket url is too long");
    }

    *address.out = '\0';
}

void OkxStreamer::initialize()
{
    m_client.initialize();
}

void OkxStreamer::tick(nanoseconds now) noexcept
{
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
    }

    subscribe();
    report(now);
}

void OkxStreamer::receive() noexcept
{
    do
    {
        std::string_view text;
        const network::WebsocketStatus status = m_client.read(text, kReadTimeout);

        if (status == network::WebsocketStatus::Closed) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Warning>("okx streamer closed by server: {}", m_client.error());
            disconnect();

            return;
        }

        if (status == network::WebsocketStatus::Failed) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Error>("okx streamer read failed: {}", m_client.error());
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

void OkxStreamer::close() noexcept
{
    if (!m_client.connected())
    {
        return;
    }

    nlx::detail::epoll_unwatch(m_epoll, m_client.fd());
    m_client.close();
}

size_t OkxStreamer::update(std::span<const OkxInstrument> instruments, std::span<uint16_t, kTouchedCapacity> touched, size_t &ignored) noexcept
{
    const auto identifier = [](const OkxInstrument &instrument) { return std::string_view(instrument.id, ::strnlen(instrument.id, OkxInstrument::kIdCapacity)); };
    const auto valid = [](std::string_view id) { return !id.empty() && id.size() < OkxInstrument::kIdCapacity && json_safe(id); };

    for (Slot &slot : m_slots)
    {
        slot.wanted = false;
    }

    for (const OkxInstrument &instrument : instruments)
    {
        const std::string_view id = identifier(instrument);
        if (!valid(id)) [[unlikely]]
        {
            continue;
        }

        const size_t index = find(id);
        if (index != kInstrumentsCapacity)
        {
            m_slots[index].wanted = true;
        }
    }

    if (m_client.connected() && !submit("unsubscribe", false)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx streamer unsubscribe skipped");
    }

    size_t count = 0;
    size_t removed = 0;

    for (size_t index = 0; index < kInstrumentsCapacity; ++index)
    {
        const Slot &slot = m_slots[index];
        if (slot.size == 0 || slot.wanted)
        {
            continue;
        }

        release(index);
        touched[count++] = static_cast<uint16_t>(index);
        ++removed;
    }

    if (removed != 0)
    {
        reindex();
    }

    size_t added = 0;
    size_t vacant = 0;
    ignored = 0;

    for (const OkxInstrument &instrument : instruments)
    {
        const std::string_view id = identifier(instrument);
        if (!valid(id)) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Warning>("okx streamer instrument ignored: id = {}", id);
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
            m_log.log<logging::LogLevel::Warning>("okx streamer instrument ignored: id = {}, capacity = {}", id, kInstrumentsCapacity);
            ++ignored;

            continue;
        }

        Slot &slot = m_slots[vacant];
        nlx::detail::str_assign(slot.id, id);
        slot.size = static_cast<uint8_t>(id.size());
        slot.wanted = true;

        insert(vacant);
        touched[count++] = static_cast<uint16_t>(vacant);
        ++added;
    }

    m_dirty = true;
    subscribe();

    m_log.log("okx streamer instruments updated: instruments = {}, removed = {}, added = {}, ignored = {}, connected = {}",
              instruments.size(), removed, added, ignored, m_client.connected());

    return count;
}

bool OkxStreamer::connected() const noexcept
{
    return m_client.connected();
}

int OkxStreamer::fd() const noexcept
{
    return m_client.fd();
}

std::string_view OkxStreamer::id(size_t index) const noexcept
{
    return name(m_slots[index]);
}

bool OkxStreamer::connect(nanoseconds now) noexcept
{
    const nanoseconds delay = m_limiter.acquire(WebsocketConnect, now);
    if (delay != nanoseconds::zero()) [[unlikely]]
    {
        m_retry = now + delay;
        m_log.log<logging::LogLevel::Warning>("okx streamer connect throttled for {} ms", nlx::detail::ms(delay));

        return false;
    }

    m_log.log("okx streamer connecting to {}", std::string_view(m_url));

    if (!m_client.connect(m_url, kConnectTimeout))
    {
        const nanoseconds end = time::now_monotonic();
        schedule(end);

        m_log.log<logging::LogLevel::Error>("okx streamer connect failed in {} ms: {}, reconnect in {} ms",
                                            nlx::detail::ms(end - now), m_client.error(), nlx::detail::ms(m_retry - end));

        return false;
    }

    if (!nlx::detail::epoll_watch(m_epoll, m_client.fd())) [[unlikely]]
    {
        m_client.close();
        schedule(time::now_monotonic());

        m_log.log<logging::LogLevel::Error>("okx streamer epoll registration failed");

        return false;
    }

    m_limiter.reset(WebsocketBusinessSubscribe);

    const nanoseconds end = time::now_monotonic();

    m_opened = end;
    m_received = end;
    m_pinged = nanoseconds::zero();
    m_resume = nanoseconds::zero();
    m_dirty = true;

    m_log.log("okx streamer connected in {} ms", nlx::detail::ms(end - now));

    subscribe();

    return true;
}

void OkxStreamer::disconnect() noexcept
{
    close();

    const nanoseconds now = time::now_monotonic();
    if (now - m_opened >= kStableInterval)
    {
        m_backoff = kReconnectDelay;
    }

    schedule(now);
    m_dirty = true;

    m_log.log<logging::LogLevel::Warning>("okx streamer disconnected after {} ms, reconnect in {} ms",
                                          nlx::detail::ms(now - m_opened), nlx::detail::ms(m_retry - now));
}

void OkxStreamer::schedule(nanoseconds now) noexcept
{
    m_retry = now + m_backoff;
    m_backoff = std::min(m_backoff * 2, kReconnectLimit);
}

void OkxStreamer::keepalive(nanoseconds now) noexcept
{
    if (m_pinged != nanoseconds::zero())
    {
        if (now - m_pinged >= kPongTimeout) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Error>("okx streamer pong timeout");
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
        m_log.log("okx streamer ping sent after {} ms idle", nlx::detail::ms(now - m_received));
    }
}

void OkxStreamer::subscribe() noexcept
{
    if (!m_dirty || !m_client.connected() || time::now_monotonic() < m_resume)
    {
        return;
    }

    m_dirty = !submit("subscribe", true);
}

bool OkxStreamer::submit(std::string_view operation, bool wanted) noexcept
{
    const auto matches = [wanted](const Slot &slot) { return slot.size != 0 && slot.wanted == wanted; };

    size_t begin = 0;
    while (begin < kInstrumentsCapacity)
    {
        const std::span<char> output = m_client.output();

        char *const data = output.data();
        char *const end = data + output.size() - request_suffix.size();

        const auto head = std::format_to_n(data, end - data, R"({{"op":"{}","args":[)", operation);
        char *out = head.out;

        size_t count = 0;
        size_t index = begin;
        for (; index < kInstrumentsCapacity; ++index)
        {
            const Slot &slot = m_slots[index];
            if (!matches(slot))
            {
                continue;
            }

            const std::string_view separator = count != 0 ? "," : "";
            const auto argument = std::format_to_n(out, end - out, R"({}{{"channel":"candle1m","instId":"{}"}})", separator, name(slot));
            if (argument.size > end - out)
            {
                break;
            }

            out = argument.out;
            ++count;
        }

        if (count == 0)
        {
            return true;
        }

        out = std::ranges::copy(request_suffix, out).out;

        const nanoseconds now = time::now_monotonic();
        const nanoseconds delay = m_limiter.acquire(WebsocketBusinessSubscribe, now);
        if (delay != nanoseconds::zero()) [[unlikely]]
        {
            m_resume = now + delay;
            m_log.log<logging::LogLevel::Warning>("okx streamer {} throttled for {} ms", operation, nlx::detail::ms(delay));

            return false;
        }

        const size_t size = static_cast<size_t>(out - data);
        if (!send(size))
        {
            return false;
        }

        m_log.log("okx streamer {} sent: channels = {}, bytes = {}", operation, count, size);

        begin = index;
    }

    return true;
}

bool OkxStreamer::send(size_t size) noexcept
{
    if (m_client.send(size)) [[likely]]
    {
        return true;
    }

    m_log.log<logging::LogLevel::Error>("okx streamer send failed: {}", m_client.error());
    disconnect();

    return false;
}

void OkxStreamer::dispatch(std::string_view text) noexcept
{
    ++m_messages;

    if (text.starts_with(candle_prefix)) [[likely]]
    {
        if (text.ends_with(forming_suffix)) [[likely]]
        {
            ++m_updates;
            return;
        }

        record(text);
        return;
    }

    if (text == pong_message)
    {
        return;
    }

    if (text.starts_with(event_prefix))
    {
        handle_event(text);
        return;
    }

    m_log.log<logging::LogLevel::Warning>("okx streamer unexpected message: {}", text);
}

void OkxStreamer::record(std::string_view text) noexcept
{
    std::string_view id;
    OkxBar bar;

    if (!parse_candle(text, id, bar)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx streamer malformed candle: {}", text);
        return;
    }

    const size_t index = find(id);
    if (index == kInstrumentsCapacity) [[unlikely]]
    {
        ++m_unknown;
        return;
    }

    ++m_closes;

    const nanoseconds latency = time::now() - (bar.time + kBarInterval);

    m_log.log("okx streamer {} candle: time = {}, open = {}, high = {}, low = {}, close = {}, volume = {}, latency = {} ms",
              id, bar.time.count(), bar.open, bar.high, bar.low, bar.close, bar.quote_volume, nlx::detail::ms(latency));

    m_handler(m_object, index, bar);
}

void OkxStreamer::handle_event(std::string_view text) noexcept
{
    if (text.size() > kEventCapacity) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx streamer event is too long: size = {}, {}", text.size(), text);
        return;
    }

    std::ranges::copy(text, m_event);

    std::string_view event;
    std::string_view instrument;
    std::string_view message;
    int code = 0;

    if (!parsing::detail::okx_parse_event(std::string_view(m_event, text.size()), event, instrument, code, message)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx streamer malformed event: {}", text);
        return;
    }

    if (event == "subscribe" || event == "unsubscribe") [[likely]]
    {
        ++m_acks;
        return;
    }

    if (event == "error")
    {
        ++m_errors;
        m_log.log<logging::LogLevel::Error>("okx streamer error: code = {}, message = {}", code, message);

        return;
    }

    if (event == "notice")
    {
        m_log.log<logging::LogLevel::Warning>("okx streamer notice: code = {}, message = {}", code, message);
        return;
    }

    m_log.log<logging::LogLevel::Warning>("okx streamer event: {}", text);
}

void OkxStreamer::report(nanoseconds now) noexcept
{
    if (now - m_reported < kReportInterval)
    {
        return;
    }

    m_log.log("okx streamer report: connected = {}, messages = {}, updates = {}, closes = {}, acks = {}, errors = {}, unknown = {}",
              m_client.connected(), m_messages, m_updates, m_closes, m_acks, m_errors, m_unknown);

    m_reported = now;
    m_messages = 0;
    m_updates = 0;
    m_closes = 0;
    m_acks = 0;
    m_errors = 0;
    m_unknown = 0;
}

void OkxStreamer::release(size_t index) noexcept
{
    Slot &slot = m_slots[index];

    slot.id[0] = '\0';
    slot.size = 0;
    slot.wanted = false;
}

void OkxStreamer::insert(size_t index) noexcept
{
    constexpr size_t mask = kIndexCapacity - 1;

    size_t position = hash(name(m_slots[index])) & mask;
    while (m_index[position] != 0)
    {
        position = (position + 1) & mask;
    }

    m_index[position] = static_cast<uint16_t>(index + 1);
}

void OkxStreamer::reindex() noexcept
{
    m_index.fill(0);

    for (size_t index = 0; index < kInstrumentsCapacity; ++index)
    {
        if (m_slots[index].size != 0)
        {
            insert(index);
        }
    }
}

size_t OkxStreamer::find(std::string_view id) const noexcept
{
    constexpr size_t mask = kIndexCapacity - 1;

    for (size_t position = hash(id) & mask; m_index[position] != 0; position = (position + 1) & mask)
    {
        const size_t index = m_index[position] - 1u;
        if (name(m_slots[index]) == id)
        {
            return index;
        }
    }

    return kInstrumentsCapacity;
}

std::string_view OkxStreamer::name(const Slot &slot) noexcept
{
    return std::string_view(slot.id, slot.size);
}

} // namespace nlx::exchange
