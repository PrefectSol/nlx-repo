#include "exchange/okx_account_streamer.h"
#include "core/crypto.h"
#include "core/string.h"
#include "core/time.h"
#include "logging/logger_node.h"
#include "parsing/json_reader.h"

#include <algorithm>
#include <chrono>
#include <format>

namespace nlx::exchange
{

OkxAccountStreamer::OkxAccountStreamer(const Credentials &credentials, OkxRateLimiter &limiter, const char *url, logging::LoggerNode &log) :
    m_log(log),
    m_limiter(limiter),
    m_url(url),
    m_epoll(),
    m_client(),
    m_key(),
    m_passphrase(),
    m_secret(),
    m_balance(),
    m_timestamp(0),
    m_positions(),
    m_positions_size(0),
    m_snapshot(false),
    m_orders(),
    m_orders_size(0),
    m_reply(),
    m_received(0),
    m_pinged_at(0),
    m_pinged(false),
    m_retry_at(0),
    m_backoff(kBackoffMilliseconds),
    m_upgrade(false)
{
    m_epoll = FileDescriptor::make_epoll();

    nlx::detail::str_assign(m_key, credentials.okx_api_key);
    nlx::detail::str_assign(m_passphrase, credentials.okx_api_passphrase);
    nlx::detail::str_assign(m_secret, credentials.okx_api_secret);
}

void OkxAccountStreamer::initialize()
{
    m_client.initialize();
}

bool OkxAccountStreamer::poll(AccountUpdateType &type) noexcept
{
    while (m_client.pending(milliseconds::zero()))
    {
        std::string_view text;
        if (!read(text)) [[unlikely]]
        {
            return false;
        }

        if (extract(text, type))
        {
            return true;
        }
    }

    return false;
}

void OkxAccountStreamer::heartbeat() noexcept
{
    if (!m_client.connected())
    {
        if (time::now_monotonic() >= m_retry_at)
        {
            [[maybe_unused]] const bool connected = reconnect();
        }

        return;
    }

    if (m_upgrade) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx account streamer reconnecting before service upgrade");

        [[maybe_unused]] const bool connected = reconnect();
        return;
    }

    const nanoseconds interval = milliseconds(kHeartbeatMilliseconds);
    const nanoseconds idle = time::now_monotonic() - m_received;

    if (idle >= 2 * interval) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx account streamer heartbeat lost: idle = {} ms", nlx::detail::ms(idle));

        [[maybe_unused]] const bool connected = reconnect();
        return;
    }

    if (idle >= interval && !m_pinged)
    {
        const std::span<char> output = m_client.output();
        const auto ping = std::format_to_n(output.data(), static_cast<uint32_t>(output.size()), "ping");

        if (send(static_cast<size_t>(ping.size)))
        {
            m_pinged_at = time::now_monotonic();
            m_pinged = true;
        }
    }
}

void OkxAccountStreamer::close() noexcept
{
    m_client.close();
}

const FileDescriptor &OkxAccountStreamer::fd() const noexcept
{
    return m_epoll;
}

bool OkxAccountStreamer::connected() const noexcept
{
    return m_client.connected();
}

const Balance &OkxAccountStreamer::balance() const noexcept
{
    return m_balance;
}

int64_t OkxAccountStreamer::timestamp() const noexcept
{
    return m_timestamp;
}

std::span<const Position> OkxAccountStreamer::positions() const noexcept
{
    return std::span<const Position>(m_positions.data(), m_positions_size);
}

bool OkxAccountStreamer::snapshot() const noexcept
{
    return m_snapshot;
}

std::span<const Order> OkxAccountStreamer::orders() const noexcept
{
    return std::span<const Order>(m_orders.data(), m_orders_size);
}

const OrderReply &OkxAccountStreamer::reply() const noexcept
{
    return m_reply;
}

bool OkxAccountStreamer::place(const OrderTicket &ticket) noexcept
{
    if (!m_client.connected()) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx account streamer order rejected: not connected, client = {}", std::string_view(ticket.client));
        return false;
    }

    const nanoseconds delay = m_limiter.acquire(WebsocketOrder, time::now_monotonic());
    if (delay != nanoseconds::zero()) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx account streamer order throttled for {} ms: client = {}", nlx::detail::ms(delay), std::string_view(ticket.client));
        return false;
    }

    constexpr std::array<std::string_view, 4> types = { "market", "limit", "post_only", "ioc" };

    const std::string_view side = ticket.buy ? "buy" : "sell";
    const std::string_view position = ticket.position == PositionSide::Long ? "long" : "short";
    const std::string_view type = types[static_cast<size_t>(ticket.type)];
    const std::string_view client(ticket.client);

    const std::span<char> output = m_client.output();
    char *const data = output.data();
    char *const end = data + output.size();

    char *out = std::format_to_n(data, end - data, R"({{"id":"{}","op":"order","args":[{{"instIdCode":{},"tdMode":"isolated","side":"{}","posSide":"{}","ordType":"{}","sz":"{}","clOrdId":"{}")",
                                 client, ticket.instrument, side, position, type, std::string_view(ticket.size), client).out;

    if (ticket.type != OrderType::Market)
    {
        out = std::format_to_n(out, end - out, R"(,"px":"{}")", std::string_view(ticket.price)).out;
    }

    out = std::format_to_n(out, end - out, "}}]}}").out;

    return send(static_cast<size_t>(out - data));
}

bool OkxAccountStreamer::cancel(int64_t instrument, std::string_view client) noexcept
{
    if (!m_client.connected()) [[unlikely]]
    {
        return false;
    }

    const nanoseconds delay = m_limiter.acquire(WebsocketCancel, time::now_monotonic());
    if (delay != nanoseconds::zero()) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx account streamer cancel throttled for {} ms: client = {}", nlx::detail::ms(delay), client);
        return false;
    }

    const std::span<char> output = m_client.output();
    const auto request = std::format_to_n(output.data(), static_cast<uint32_t>(output.size()),
                                          R"({{"id":"c{}","op":"cancel-order","args":[{{"instIdCode":{},"clOrdId":"{}"}}]}})", client, instrument, client);

    return send(static_cast<size_t>(request.size));
}

bool OkxAccountStreamer::reconnect() noexcept
{
    close();

    if (connect())
    {
        m_backoff = milliseconds(kBackoffMilliseconds);
        return true;
    }

    m_retry_at = time::now_monotonic() + m_backoff;

    m_log.log<logging::LogLevel::Warning>("okx account streamer reconnect failed: retry in {} ms", m_backoff.count());

    m_backoff = std::min(m_backoff * 2, milliseconds(kBackoffLimitMilliseconds));

    return false;
}

bool OkxAccountStreamer::connect() noexcept
{
    acquire(WebsocketConnect);

    const nanoseconds begin = time::now_monotonic();
    if (!m_client.connect(m_url, milliseconds(kConnectMilliseconds)))
    {
        m_log.log<logging::LogLevel::Error>("okx account streamer connect failed: {}", m_client.error());
        return false;
    }

    if (!nlx::detail::epoll_watch(m_epoll, m_client.fd())) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx account streamer epoll registration failed");
        close();

        return false;
    }

    m_received = time::now_monotonic();
    m_pinged = false;
    m_upgrade = false;

    m_log.log("okx account streamer connected in {} ms", nlx::detail::ms(m_received - begin));

    if (!login())
    {
        close();
        return false;
    }

    return subscribe();
}

bool OkxAccountStreamer::login() noexcept
{
    acquire(WebsocketPrivateSubscribe);

    const int64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(time::now()).count();

    char stamp[32];
    const auto written = std::format_to_n(stamp, sizeof(stamp) - 1, "{}", seconds);
    const std::string_view timestamp(stamp, static_cast<size_t>(written.size));

    char message[64];
    const auto prepared = std::format_to_n(message, sizeof(message) - 1, "{}GET/users/self/verify", timestamp);

    char signature[crypto::hmac_sha256_base64_size];
    crypto::hmac_sha256_base64(signature, m_secret, std::string_view(message, static_cast<size_t>(prepared.size)));

    const std::span<char> output = m_client.output();
    const auto request = std::format_to_n(output.data(), static_cast<uint32_t>(output.size()),
                                          R"({{"op":"login","args":[{{"apiKey":"{}","passphrase":"{}","timestamp":"{}","sign":"{}"}}]}})",
                                          std::string_view(m_key), std::string_view(m_passphrase), timestamp, std::string_view(signature));

    if (!send(static_cast<size_t>(request.size)))
    {
        return false;
    }

    const nanoseconds begin = time::now_monotonic();
    while (true)
    {
        const nanoseconds remaining = milliseconds(kLoginMilliseconds) - (time::now_monotonic() - begin);
        if (remaining <= nanoseconds::zero() || !m_client.pending(nlx::detail::ceil_milliseconds(remaining)))
        {
            m_log.log<logging::LogLevel::Error>("okx account streamer login timed out");
            return false;
        }

        std::string_view text;
        if (!read(text))
        {
            return false;
        }

        std::string_view event;
        std::string_view instrument;
        int code = 0;
        std::string_view reason;

        const bool parsed = text.starts_with(R"({"event":)") && parsing::detail::okx_parse_event(text, event, instrument, code, reason);
        if (!parsed)
        {
            dispatch(text);
            continue;
        }

        const bool rejected = event == "error" || (event == "login" && code != 0);
        if (rejected)
        {
            m_log.log<logging::LogLevel::Error>("okx account streamer login failed: code = {}, message = {}", code, reason);
            return false;
        }

        if (event == "login")
        {
            m_log.log("okx account streamer logged in in {} ms", nlx::detail::ms(time::now_monotonic() - begin));
            return true;
        }

        dispatch(text);
    }
}

bool OkxAccountStreamer::subscribe() noexcept
{
    acquire(WebsocketPrivateSubscribe);

    const std::span<char> output = m_client.output();
    const auto request = std::format_to_n(output.data(), static_cast<uint32_t>(output.size()),
                                          R"({{"op":"subscribe","args":[{{"channel":"account","ccy":"{}"}},{{"channel":"positions","instType":"SWAP"}},{{"channel":"orders","instType":"SWAP"}}]}})",
                                          kCurrency);

    return send(static_cast<size_t>(request.size));
}

void OkxAccountStreamer::acquire(OkxRequest request) noexcept
{
    nanoseconds delay = m_limiter.acquire(request, time::now_monotonic());
    while (delay != nanoseconds::zero())
    {
        m_log.log<logging::LogLevel::Warning>("okx account streamer throttled for {} ms", nlx::detail::ms(delay));

        time::sleep(delay);
        delay = m_limiter.acquire(request, time::now_monotonic());
    }
}

bool OkxAccountStreamer::send(size_t size) noexcept
{
    if (m_client.send(size)) [[likely]]
    {
        return true;
    }

    m_log.log<logging::LogLevel::Error>("okx account streamer send failed: {}", m_client.error());
    close();

    return false;
}

bool OkxAccountStreamer::read(std::string_view &text) noexcept
{
    const network::WebsocketStatus status = m_client.read(text, milliseconds(kReadMilliseconds));

    if (status == network::WebsocketStatus::Closed) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx account streamer closed by server: {}", m_client.error());
        close();

        return false;
    }

    if (status == network::WebsocketStatus::Failed) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx account streamer read failed: {}", m_client.error());
        close();

        return false;
    }

    m_received = time::now_monotonic();
    m_pinged = false;

    return true;
}

bool OkxAccountStreamer::extract(std::string_view text, AccountUpdateType &type) noexcept
{
    if (text.starts_with(R"({"id":)"))
    {
        const bool parsed = parsing::detail::okx_parse_reply(text, m_reply);
        if (!parsed) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Error>("okx account streamer reply parse failed: {}", text);
            return false;
        }

        type = AccountUpdateType::Reply;
        return true;
    }

    std::string_view channel;
    if (!parsing::detail::okx_parse_channel(text, channel))
    {
        dispatch(text);
        return false;
    }

    if (channel == "account")
    {
        const bool parsed = parsing::detail::okx_parse_account_push(text, kCurrency, m_balance, m_timestamp);
        if (!parsed) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Error>("okx account streamer account parse failed: {}", text);
            return false;
        }

        type = AccountUpdateType::Balance;
        return true;
    }

    if (channel == "positions")
    {
        const bool parsed = parsing::detail::okx_parse_positions_push(text, m_positions, m_positions_size, m_snapshot);
        if (!parsed) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Error>("okx account streamer positions parse failed: {}", text);
            return false;
        }

        type = AccountUpdateType::Positions;
        return true;
    }

    if (channel == "orders")
    {
        const bool parsed = parsing::detail::okx_parse_orders_push(text, m_orders, m_orders_size);
        if (!parsed) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Error>("okx account streamer orders parse failed: {}", text);
            return false;
        }

        type = AccountUpdateType::Orders;
        return true;
    }

    m_log.log<logging::LogLevel::Warning>("okx account streamer unexpected channel: {}", channel);

    return false;
}

void OkxAccountStreamer::dispatch(std::string_view text) noexcept
{
    if (text.empty())
    {
        return;
    }

    if (text == "pong")
    {
        m_pinged = false;

        const double rtt = nlx::detail::ms(time::now_monotonic() - m_pinged_at);
        m_log.log<logging::LogLevel::Debug>("okx account streamer pong received: rtt = {}", rtt);

        return;
    }

    if (!text.starts_with(R"({"event":)")) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx account streamer unexpected message: {}", text);
        return;
    }

    std::string_view event;
    std::string_view instrument;
    int code = 0;
    std::string_view message;

    if (!parsing::detail::okx_parse_event(text, event, instrument, code, message)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx account streamer event parse failed: {}", text);
        return;
    }

    if (event == "subscribe")
    {
        m_log.log("okx account streamer subscribed: {}", text);
        return;
    }

    if (event == "channel-conn-count")
    {
        m_log.log<logging::LogLevel::Debug>("okx account streamer connections: {}", text);
        return;
    }

    if (event == "error")
    {
        m_log.log<logging::LogLevel::Error>("okx account streamer error: code = {}, message = {}", code, message);
        return;
    }

    if (event == "notice")
    {
        m_log.log<logging::LogLevel::Warning>("okx account streamer notice: code = {}, message = {}", code, message);
        m_upgrade = m_upgrade || code == 64008;

        return;
    }

    m_log.log<logging::LogLevel::Warning>("okx account streamer event: {}", text);
}

} // namespace nlx::exchange
