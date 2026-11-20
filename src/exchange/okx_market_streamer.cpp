#include "exchange/okx_market_streamer.h"
#include "core/string.h"
#include "core/time.h"
#include "logging/logger_node.h"
#include "parsing/json_reader.h"

#include <algorithm>
#include <bit>
#include <format>

namespace nlx::exchange
{

namespace
{

constexpr uint32_t bit(size_t slot) noexcept
{
    return uint32_t{ 1 } << slot;
}

} // namespace

OkxMarketStreamer::OkxMarketStreamer(OkxRateLimiter &limiter, const char *url, size_t clock_milliseconds, logging::LoggerNode &log) :
    m_log(log),
    m_limiter(limiter),
    m_url(url),
    m_clock_milliseconds(clock_milliseconds),
    m_epoll(),
    m_client(),
    m_books(),
    m_slots(),
    m_failures(0),
    m_trades(),
    m_trades_slot(0),
    m_clock_at(0),
    m_clock_phase(0),
    m_phase_warned(false),
    m_retry_at(0),
    m_backoff(kBackoffMilliseconds),
    m_upgrade(false)
{
    m_epoll = FileDescriptor::make_epoll();
    m_slots.fill(kBooksCapacity);

    for (Book &book : m_books)
    {
        book.slot = kSlotsCapacity;
    }
}

void OkxMarketStreamer::initialize()
{
    m_client.initialize();
}

bool OkxMarketStreamer::subscribe(size_t slot, std::string_view instrument) noexcept
{
    if (!m_client.connected()) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx market streamer subscribe rejected: not connected, instrument = {}", instrument);
        return false;
    }

    const nanoseconds delay = m_limiter.acquire(WebsocketPublicSubscribe, time::now_monotonic());
    if (delay != nanoseconds::zero()) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx market streamer subscribe throttled for {} ms: instrument = {}", nlx::detail::ms(delay), instrument);
        return false;
    }

    const bool shared = instrument == kClock;
    const size_t index = shared ? 0 : slot + 1;

    Book &book = m_books[index];
    if (!shared)
    {
        nlx::detail::str_assign(book.id, instrument);
        book.levels.clear();
        book.sequence = 0;
        book.ready = false;
    }

    if (!request("subscribe", instrument, !shared, true))
    {
        return false;
    }

    book.slot = slot;
    book.active = true;
    m_slots[slot] = index;

    return true;
}

void OkxMarketStreamer::unsubscribe(size_t slot) noexcept
{
    m_failures &= ~bit(slot);

    const size_t index = m_slots[slot];
    if (index == kBooksCapacity)
    {
        return;
    }

    m_slots[slot] = kBooksCapacity;

    if (m_trades_slot == slot)
    {
        m_trades = std::string_view();
    }

    const bool shared = index == 0;

    Book &book = m_books[index];
    book.slot = kSlotsCapacity;
    book.active = shared;
    book.ready = shared && book.ready;

    if (!m_client.connected())
    {
        return;
    }

    const nanoseconds delay = m_limiter.acquire(WebsocketPublicSubscribe, time::now_monotonic());
    if (delay != nanoseconds::zero()) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx market streamer unsubscribe throttled: instrument = {} stays subscribed", std::string_view(book.id));
        return;
    }

    [[maybe_unused]] const bool sent = request("unsubscribe", book.id, !shared, true);
}

bool OkxMarketStreamer::poll(MarketUpdate &update) noexcept
{
    while (true)
    {
        if (m_failures != 0) [[unlikely]]
        {
            const size_t slot = static_cast<size_t>(std::countr_zero(m_failures));
            m_failures &= m_failures - 1;

            update = MarketUpdate{ MarketUpdateType::Failure, slot, 0, 0.0, 0.0 };
            return true;
        }

        if (!m_trades.empty())
        {
            if (next_trade(update)) [[likely]]
            {
                return true;
            }

            continue;
        }

        if (!m_client.pending(milliseconds::zero()))
        {
            return false;
        }

        std::string_view text;
        if (!read(text)) [[unlikely]]
        {
            drop();
            continue;
        }

        std::string_view channel;
        std::string_view instrument;
        std::string_view body;

        const bool market = parsing::detail::okx_parse_market_head(text, channel, instrument, body);
        if (!market) [[unlikely]]
        {
            dispatch(text);
            continue;
        }

        const size_t index = find(instrument);
        if (index == kBooksCapacity) [[unlikely]]
        {
            continue;
        }

        if (channel == "books")
        {
            if (apply(index, body, update)) [[likely]]
            {
                return true;
            }

            continue;
        }

        if (channel != "trades") [[unlikely]]
        {
            m_log.log<logging::LogLevel::Warning>("okx market streamer unexpected channel: channel = {}, instrument = {}", channel, instrument);
            continue;
        }

        const size_t slot = m_books[index].slot;
        if (slot == kSlotsCapacity) [[unlikely]]
        {
            continue;
        }

        if (!parsing::detail::okx_parse_trades(body, m_trades)) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Error>("okx market streamer trades parse failed: {}", text);

            m_trades = std::string_view();
            m_failures |= bit(slot);

            continue;
        }

        m_trades_slot = slot;
    }
}

void OkxMarketStreamer::heartbeat() noexcept
{
    if (!m_client.connected())
    {
        if (time::now_monotonic() >= m_retry_at)
        {
            [[maybe_unused]] const bool connected = reconnect();
        }

        return;
    }

    if (m_upgrade && idle()) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx market streamer reconnecting before service upgrade");

        drop();
        [[maybe_unused]] const bool connected = reconnect();

        return;
    }

    const nanoseconds silence = time::now_monotonic() - m_clock_at;
    if (silence >= milliseconds(m_clock_milliseconds)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx market streamer clock lost: silence = {} ms", nlx::detail::ms(silence));

        drop();
        [[maybe_unused]] const bool connected = reconnect();

        return;
    }

    const bool shifted = m_clock_phase > kPhaseMilliseconds;
    if (shifted && !m_phase_warned) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx market streamer clock phase shifted: phase = {} ms, seconds close later by this amount", m_clock_phase);
    }

    m_phase_warned = shifted;
}

void OkxMarketStreamer::close() noexcept
{
    m_client.close();
}

const FileDescriptor &OkxMarketStreamer::fd() const noexcept
{
    return m_epoll;
}

bool OkxMarketStreamer::connected() const noexcept
{
    return m_client.connected();
}

bool OkxMarketStreamer::alive() const noexcept
{
    return m_client.connected() && m_books[0].ready;
}

bool OkxMarketStreamer::ready(size_t slot) const noexcept
{
    const size_t index = m_slots[slot];
    return index != kBooksCapacity && m_books[index].ready;
}

double OkxMarketStreamer::bid(size_t slot) const noexcept
{
    const size_t index = m_slots[slot];
    return index != kBooksCapacity ? m_books[index].levels.best_bid() : 0.0;
}

double OkxMarketStreamer::ask(size_t slot) const noexcept
{
    const size_t index = m_slots[slot];
    return index != kBooksCapacity ? m_books[index].levels.best_ask() : 0.0;
}

bool OkxMarketStreamer::reconnect() noexcept
{
    if (connect())
    {
        m_backoff = milliseconds(kBackoffMilliseconds);
        return true;
    }

    m_retry_at = time::now_monotonic() + m_backoff;

    m_log.log<logging::LogLevel::Warning>("okx market streamer reconnect failed: retry in {} ms", m_backoff.count());

    m_backoff = std::min(m_backoff * 2, milliseconds(kBackoffLimitMilliseconds));

    return false;
}

bool OkxMarketStreamer::connect() noexcept
{
    nanoseconds delay = m_limiter.acquire(WebsocketConnect, time::now_monotonic());
    while (delay != nanoseconds::zero())
    {
        m_log.log<logging::LogLevel::Warning>("okx market streamer throttled for {} ms", nlx::detail::ms(delay));

        time::sleep(delay);
        delay = m_limiter.acquire(WebsocketConnect, time::now_monotonic());
    }

    const nanoseconds begin = time::now_monotonic();
    if (!m_client.connect(m_url, milliseconds(kConnectMilliseconds)))
    {
        m_log.log<logging::LogLevel::Error>("okx market streamer connect failed: {}", m_client.error());
        return false;
    }

    if (!nlx::detail::epoll_watch(m_epoll, m_client.fd())) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx market streamer epoll registration failed");
        m_client.close();

        return false;
    }

    m_clock_at = time::now_monotonic();
    m_upgrade = false;

    Book &clock = m_books[0];
    nlx::detail::str_assign(clock.id, kClock);
    clock.levels.clear();
    clock.sequence = 0;
    clock.slot = kSlotsCapacity;
    clock.active = true;
    clock.ready = false;

    m_log.log("okx market streamer connected in {} ms", nlx::detail::ms(m_clock_at - begin));

    delay = m_limiter.acquire(WebsocketPublicSubscribe, time::now_monotonic());
    if (delay != nanoseconds::zero()) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx market streamer clock subscribe throttled for {} ms", nlx::detail::ms(delay));
        drop();

        return false;
    }

    return request("subscribe", kClock, true, false);
}

void OkxMarketStreamer::drop() noexcept
{
    m_client.close();

    for (size_t slot = 0; slot < kSlotsCapacity; ++slot)
    {
        if (m_slots[slot] != kBooksCapacity)
        {
            m_failures |= bit(slot);
            m_slots[slot] = kBooksCapacity;
        }
    }

    for (Book &book : m_books)
    {
        book.slot = kSlotsCapacity;
        book.active = false;
        book.ready = false;
    }

    m_trades = std::string_view();
}

bool OkxMarketStreamer::request(std::string_view operation, std::string_view instrument, bool books, bool trades) noexcept
{
    const std::span<char> output = m_client.output();

    char *const data = output.data();
    char *const end = data + output.size();

    char *out = std::format_to_n(data, end - data, R"({{"op":"{}","args":[)", operation).out;

    if (books)
    {
        out = std::format_to_n(out, end - out, R"({{"channel":"books","instId":"{}"}})", instrument).out;
    }

    if (books && trades)
    {
        *out++ = ',';
    }

    if (trades)
    {
        out = std::format_to_n(out, end - out, R"({{"channel":"trades","instId":"{}"}})", instrument).out;
    }

    out = std::format_to_n(out, end - out, "]}}").out;

    return send(static_cast<size_t>(out - data));
}

bool OkxMarketStreamer::send(size_t size) noexcept
{
    if (m_client.send(size)) [[likely]]
    {
        return true;
    }

    m_log.log<logging::LogLevel::Error>("okx market streamer send failed: {}", m_client.error());
    drop();

    return false;
}

bool OkxMarketStreamer::read(std::string_view &text) noexcept
{
    const network::WebsocketStatus status = m_client.read(text, milliseconds(kReadMilliseconds));

    if (status == network::WebsocketStatus::Closed) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx market streamer closed by server: {}", m_client.error());
        return false;
    }

    if (status == network::WebsocketStatus::Failed) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx market streamer read failed: {}", m_client.error());
        return false;
    }

    return true;
}

bool OkxMarketStreamer::next_trade(MarketUpdate &update) noexcept
{
    Trade trade{};

    const bool parsed = parsing::detail::okx_parse_trade(m_trades, trade);
    if (!parsed) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx market streamer trade parse failed: slot = {}", m_trades_slot);

        m_trades = std::string_view();
        m_failures |= bit(m_trades_slot);

        return false;
    }

    update = MarketUpdate{ MarketUpdateType::Trade, m_trades_slot, trade.timestamp, trade.price, trade.size };

    return true;
}

bool OkxMarketStreamer::apply(size_t index, std::string_view body, MarketUpdate &update) noexcept
{
    Book &book = m_books[index];
    const std::string_view id(book.id);

    bool snapshot = false;
    std::string_view asks;
    std::string_view bids;
    int64_t timestamp = 0;
    int64_t sequence = 0;
    int64_t previous = 0;

    const bool parsed = parsing::detail::okx_parse_books(body, snapshot, asks, bids, timestamp, sequence, previous);
    if (!parsed) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx market streamer books parse failed: instrument = {}", id);
        resync(index);

        return false;
    }

    if (!snapshot && !book.ready) [[unlikely]]
    {
        return false;
    }

    if (!snapshot && previous != book.sequence) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx market streamer books sequence broken: instrument = {}, expected = {}, previous = {}",
                                              id, book.sequence, previous);
        resync(index);

        return false;
    }

    if (snapshot)
    {
        book.levels.clear();
    }

    const bool applied = apply_levels(book.levels, asks, false) && apply_levels(book.levels, bids, true);
    if (!applied || book.levels.crossed()) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx market streamer books corrupted: instrument = {}, applied = {}, bid = {}, ask = {}",
                                            id, applied, book.levels.best_bid(), book.levels.best_ask());
        resync(index);

        return false;
    }

    book.sequence = sequence;
    book.ready = true;

    if (index == 0)
    {
        m_clock_at = time::now_monotonic();
    }

    if (index == 0 && !snapshot)
    {
        m_clock_phase = timestamp % 100;
    }

    const bool owned = book.slot != kSlotsCapacity;
    update = MarketUpdate{ owned ? MarketUpdateType::Book : MarketUpdateType::Clock, book.slot, timestamp, 0.0, 0.0 };

    return true;
}

bool OkxMarketStreamer::apply_levels(OrderBook &book, std::string_view levels, bool bids) noexcept
{
    Level level{};

    while (!levels.empty())
    {
        if (!parsing::detail::okx_parse_level(levels, level)) [[unlikely]]
        {
            return false;
        }

        if (bids)
        {
            book.bid(level.price, level.size);
        }
        else
        {
            book.ask(level.price, level.size);
        }
    }

    return true;
}

void OkxMarketStreamer::resync(size_t index) noexcept
{
    Book &book = m_books[index];
    book.ready = false;

    if (book.slot != kSlotsCapacity)
    {
        m_failures |= bit(book.slot);
    }

    if (index != 0)
    {
        return;
    }

    const nanoseconds now = time::now_monotonic();

    bool allowed = m_limiter.acquire(WebsocketPublicSubscribe, now) == nanoseconds::zero();
    allowed = allowed && m_limiter.acquire(WebsocketPublicSubscribe, now) == nanoseconds::zero();

    if (!allowed) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx market streamer clock resubscribe throttled");
        return;
    }

    const bool unsubscribed = request("unsubscribe", kClock, true, false);
    [[maybe_unused]] const bool subscribed = unsubscribed && request("subscribe", kClock, true, false);
}

void OkxMarketStreamer::dispatch(std::string_view text) noexcept
{
    if (text.empty())
    {
        return;
    }

    if (!text.starts_with(R"({"event":)")) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx market streamer unexpected message: {}", text);
        return;
    }

    std::string_view event;
    std::string_view instrument;
    int code = 0;
    std::string_view message;

    if (!parsing::detail::okx_parse_event(text, event, instrument, code, message)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx market streamer event parse failed: {}", text);
        return;
    }

    if (event == "subscribe" || event == "unsubscribe")
    {
        m_log.log<logging::LogLevel::Debug>("okx market streamer {}: {}", event, text);
        return;
    }

    if (event == "error")
    {
        m_log.log<logging::LogLevel::Error>("okx market streamer error: code = {}, message = {}", code, message);

        for (size_t slot = 0; slot < kSlotsCapacity; ++slot)
        {
            const size_t index = m_slots[slot];
            if (index != kBooksCapacity && !m_books[index].ready)
            {
                m_failures |= bit(slot);
            }
        }

        return;
    }

    if (event == "notice")
    {
        m_log.log<logging::LogLevel::Warning>("okx market streamer notice: code = {}, message = {}", code, message);
        m_upgrade = m_upgrade || code == 64008;

        return;
    }

    m_log.log<logging::LogLevel::Warning>("okx market streamer event: {}", text);
}

size_t OkxMarketStreamer::find(std::string_view id) const noexcept
{
    for (size_t index = 0; index < kBooksCapacity; ++index)
    {
        const Book &book = m_books[index];
        if (book.active && std::string_view(book.id) == id)
        {
            return index;
        }
    }

    return kBooksCapacity;
}

bool OkxMarketStreamer::idle() const noexcept
{
    for (const size_t index : m_slots)
    {
        if (index != kBooksCapacity)
        {
            return false;
        }
    }

    return true;
}

} // namespace nlx::exchange
