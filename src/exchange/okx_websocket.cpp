#include "exchange/okx_websocket.h"
#include "core/crypto.h"
#include "core/fatal.h"
#include "core/string.h"
#include "core/time.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <stdexcept>

namespace nlx::exchange
{

namespace
{

constexpr bool json_safe(std::string_view value) noexcept
{
    return std::ranges::none_of(value, [](char ch) { return ch == '"' || ch == '\\' || static_cast<unsigned char>(ch) < 0x20; });
}

template<size_t N>
std::string_view field(const char (&value)[N]) noexcept
{
    return std::string_view(value, ::strnlen(value, N));
}

} // namespace

OkxWebsocket::OkxWebsocket(const OkxConfig &config, const Credentials &credentials, OkxRateLimiter &limiter, logging::LoggerRegistry &registry) :
    m_log(),
    m_limiter(limiter),
    m_epoll(),
    m_stop(),
    m_update(),
    m_timer(),
    m_initialized(false),
    m_key(),
    m_passphrase(),
    m_secret(),
    m_business(),
    m_private(),
    m_pending(),
    m_pending_size(0),
    m_pending_ready(false),
    m_subscriptions(),
    m_subscriptions_size(0),
    m_dirty(false),
    m_resume(0),
    m_request()
{
    m_log = registry.create_node("okx_websocket");

    const std::string_view url = field(config.websocket_url);
    if (url.empty()) [[unlikely]]
    {
        throw std::invalid_argument("okx websocket url is invalid");
    }

    const auto business_url = std::format_to_n(m_business.url, kUrlCapacity - 1, "{}{}", url, kBusinessPath);
    if (static_cast<size_t>(business_url.size) >= kUrlCapacity) [[unlikely]]
    {
        throw std::invalid_argument("okx websocket business url is too long");
    }

    *business_url.out = '\0';

    const auto private_url = std::format_to_n(m_private.url, kUrlCapacity - 1, "{}{}", url, kPrivatePath);
    if (static_cast<size_t>(private_url.size) >= kUrlCapacity) [[unlikely]]
    {
        throw std::invalid_argument("okx websocket private url is too long");
    }

    *private_url.out = '\0';

    const std::string_view key = field(credentials.api_key);
    const std::string_view passphrase = field(credentials.api_passphrase);
    if (!json_safe(key) || !json_safe(passphrase)) [[unlikely]]
    {
        throw std::invalid_argument("okx api key or passphrase contains invalid characters");
    }

    nlx::detail::str_assign(m_key, key);
    nlx::detail::str_assign(m_passphrase, passphrase);
    nlx::detail::str_assign(m_secret, field(credentials.api_secret));

    m_business.name = "business";
    m_business.request = WebsocketPublicSubscribe;
    m_business.backoff = kReconnectDelay;

    m_private.name = "private";
    m_private.request = WebsocketPrivateSubscribe;
    m_private.backoff = kReconnectDelay;

    m_epoll = FileDescriptor::make_epoll();
    m_stop = FileDescriptor::make_eventfd(true);
    m_update = FileDescriptor::make_eventfd(true);
    m_timer = FileDescriptor::make_timerfd(kTickInterval, true);

    nlx::detail::epoll_add(m_epoll, m_stop);
    nlx::detail::epoll_add(m_epoll, m_update);
    nlx::detail::epoll_add(m_epoll, m_timer);
}

void OkxWebsocket::initialize()
{
    m_business.client.initialize(m_log);
    m_private.client.initialize(m_log);

    m_initialized.store(true, std::memory_order_release);
}

void OkxWebsocket::request_stop() noexcept
{
    nlx::detail::notify(m_stop);
}

bool OkxWebsocket::resubscribe(std::span<const OkxInstrument> instruments) noexcept
{
    if (instruments.size() > kInstrumentsCapacity) [[unlikely]]
    {
        return false;
    }

    if (m_pending_ready.load(std::memory_order_acquire)) [[unlikely]]
    {
        return false;
    }

    std::copy(instruments.begin(), instruments.end(), m_pending.begin());
    m_pending_size = instruments.size();

    m_pending_ready.store(true, std::memory_order_release);
    nlx::detail::notify(m_update);

    return true;
}

void OkxWebsocket::start() noexcept
{
    if (!m_initialized.load(std::memory_order_acquire)) [[unlikely]]
    {
        NLX_FATAL("nlx::exchange::OkxWebsocket::start() called before initialize()");
    }

    m_log.log("okx websocket started");
    m_log.flush();

    update();
    tick();

    epoll_event event{};
    while (true)
    {
        const size_t count = nlx::detail::epoll_wait(m_epoll, { &event, 1 }, -1);
        if (count == 0)
        {
            continue;
        }

        if (event.data.fd == m_stop)
        {
            [[maybe_unused]] const uint64_t result = nlx::detail::drain(m_stop);
            break;
        }

        if (event.data.fd == m_update)
        {
            [[maybe_unused]] const uint64_t result = nlx::detail::drain(m_update);

            update();
            reconcile();

            m_log.flush();
        }
        else if (event.data.fd == m_timer)
        {
            [[maybe_unused]] const uint64_t ticks = nlx::detail::drain(m_timer);
            tick();
        }
        else if (event.data.fd == m_business.client.fd())
        {
            receive(m_business);
        }
        else if (event.data.fd == m_private.client.fd())
        {
            receive(m_private);
        }
    }

    close(m_business);
    close(m_private);

    m_log.log("okx websocket stopped");
    m_log.flush();
}

void OkxWebsocket::tick() noexcept
{
    if (!m_business.client.connected() && time::now_monotonic() >= m_business.retry && connect(m_business))
    {
        reset();
    }

    if (!m_private.client.connected() && time::now_monotonic() >= m_private.retry && connect(m_private))
    {
        login();
    }

    keepalive(m_business);
    keepalive(m_private);

    if (m_dirty)
    {
        reconcile();
    }

    m_log.flush();
}

void OkxWebsocket::update() noexcept
{
    if (!m_pending_ready.load(std::memory_order_acquire))
    {
        return;
    }

    size_t size = 0;
    for (size_t index = 0; index < m_subscriptions_size; ++index)
    {
        Subscription &subscription = m_subscriptions[index];
        subscription.wanted = false;

        if (subscription.active)
        {
            m_subscriptions[size++] = subscription;
        }
    }

    m_subscriptions_size = size;

    size_t ignored = 0;
    const std::span<const OkxInstrument> instruments(m_pending.data(), m_pending_size);
    for (const OkxInstrument &instrument : instruments)
    {
        const std::string_view id = field(instrument.id);

        const auto begin = m_subscriptions.begin();
        const auto end = begin + static_cast<std::ptrdiff_t>(m_subscriptions_size);
        const auto found = std::find_if(begin, end, [id](const Subscription &subscription) { return field(subscription.id) == id; });
        if (found != end)
        {
            found->wanted = true;
            continue;
        }

        if (id.empty() || m_subscriptions_size == kSubscriptionsCapacity) [[unlikely]]
        {
            ++ignored;
            continue;
        }

        Subscription &subscription = m_subscriptions[m_subscriptions_size++];
        nlx::detail::str_assign(subscription.id, id);
        subscription.wanted = true;
        subscription.active = false;
    }

    m_pending_ready.store(false, std::memory_order_release);
    m_dirty = true;

    m_log.log("okx websocket subscriptions updated: instruments = {}, ignored = {}", instruments.size(), ignored);
}

void OkxWebsocket::reset() noexcept
{
    size_t size = 0;
    for (size_t index = 0; index < m_subscriptions_size; ++index)
    {
        Subscription &subscription = m_subscriptions[index];
        subscription.active = false;

        if (subscription.wanted)
        {
            m_subscriptions[size++] = subscription;
        }
    }

    m_subscriptions_size = size;
    m_dirty = true;
    m_resume = nanoseconds::zero();
}

void OkxWebsocket::reconcile() noexcept
{
    if (!m_business.client.connected() || time::now_monotonic() < m_resume)
    {
        return;
    }

    m_dirty = !apply("unsubscribe", false) || !apply("subscribe", true);
}

bool OkxWebsocket::apply(std::string_view operation, bool wanted) noexcept
{
    Connection &connection = m_business;

    size_t begin = 0;
    while (begin < m_subscriptions_size)
    {
        char *const data = m_request;
        char *const end = data + kRequestCapacity - 2;

        const auto head = std::format_to_n(data, end - data, R"({{"op":"{}","args":[)", operation);
        if (head.size > end - data) [[unlikely]]
        {
            return false;
        }

        char *out = head.out;

        size_t count = 0;
        size_t index = begin;
        for (; index < m_subscriptions_size; ++index)
        {
            const Subscription &subscription = m_subscriptions[index];
            if (subscription.wanted != wanted || subscription.active == wanted)
            {
                continue;
            }

            const std::string_view separator = count != 0 ? "," : "";
            const auto argument = std::format_to_n(out, end - out, R"({}{{"channel":"{}","instId":"{}"}})",
                                                   separator, kCandleChannel, field(subscription.id));
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

        *out++ = ']';
        *out++ = '}';

        const nanoseconds now = time::now_monotonic();
        const nanoseconds delay = m_limiter.acquire(connection.request, now);
        if (delay != nanoseconds::zero())
        {
            m_resume = now + delay;
            m_log.log<logging::LogLevel::Warning>("okx websocket {} {} throttled for {} ms", connection.name, operation, nlx::detail::ms(delay));

            return false;
        }

        if (!send(connection, std::string_view(data, out)))
        {
            return false;
        }

        for (size_t position = begin; position < index; ++position)
        {
            Subscription &subscription = m_subscriptions[position];
            if (subscription.wanted == wanted)
            {
                subscription.active = wanted;
            }
        }

        m_log.log("okx websocket {} {} sent: channels = {}, bytes = {}", connection.name, operation, count, out - data);

        begin = index;
    }

    return true;
}

bool OkxWebsocket::connect(Connection &connection) noexcept
{
    const nanoseconds begin = time::now_monotonic();

    const nanoseconds delay = m_limiter.acquire(WebsocketConnect, begin);
    if (delay != nanoseconds::zero())
    {
        connection.retry = begin + delay;
        m_log.log("okx websocket {} connect throttled for {} ms", connection.name, nlx::detail::ms(delay));

        return false;
    }

    m_log.log("okx websocket {} connecting to {}", connection.name, std::string_view(connection.url));
    m_log.flush();

    if (!connection.client.connect(connection.url, kConnectTimeout))
    {
        const nanoseconds now = time::now_monotonic();
        schedule(connection, now);

        m_log.log<logging::LogLevel::Error>("okx websocket {} connect failed in {} ms: {}",
                                            connection.name, nlx::detail::ms(now - begin), connection.client.error());
        m_log.log<logging::LogLevel::Warning>("okx websocket {} reconnect in {} ms", connection.name, nlx::detail::ms(connection.retry - now));
        m_log.flush();

        return false;
    }

    if (!nlx::detail::epoll_watch(m_epoll, connection.client.fd())) [[unlikely]]
    {
        connection.client.close();
        schedule(connection, time::now_monotonic());

        m_log.log<logging::LogLevel::Error>("okx websocket {} epoll registration failed", connection.name);
        m_log.flush();

        return false;
    }

    const nanoseconds end = time::now_monotonic();

    connection.opened = end;
    connection.received = end;
    connection.pinged = nanoseconds::zero();

    m_log.log("okx websocket {} connected in {} ms", connection.name, nlx::detail::ms(end - begin));
    m_log.flush();

    return true;
}

void OkxWebsocket::disconnect(Connection &connection) noexcept
{
    close(connection);

    const nanoseconds now = time::now_monotonic();
    if (now - connection.opened >= kStableInterval)
    {
        connection.backoff = kReconnectDelay;
    }

    schedule(connection, now);

    m_log.log<logging::LogLevel::Warning>("okx websocket {} disconnected, reconnect in {} ms", connection.name, nlx::detail::ms(connection.retry - now));
    m_log.flush();
}

void OkxWebsocket::close(Connection &connection) noexcept
{
    if (!connection.client.connected())
    {
        return;
    }

    nlx::detail::epoll_unwatch(m_epoll, connection.client.fd());
    connection.client.close();
}

void OkxWebsocket::schedule(Connection &connection, nanoseconds now) noexcept
{
    connection.retry = now + connection.backoff;
    connection.backoff = std::min(connection.backoff * 2, kReconnectLimit);
}

void OkxWebsocket::login() noexcept
{
    Connection &connection = m_private;

    const nanoseconds delay = m_limiter.acquire(connection.request, time::now_monotonic());
    if (delay != nanoseconds::zero())
    {
        m_log.log<logging::LogLevel::Warning>("okx websocket {} login throttled for {} ms", connection.name, nlx::detail::ms(delay));
        disconnect(connection);

        return;
    }

    const seconds timestamp = nlx::detail::floor_seconds(time::now());

    char prehash[64];
    const auto message = std::format_to_n(prehash, sizeof(prehash), "{}GET/users/self/verify", timestamp.count());

    char signature[crypto::hmac_sha256_base64_size];
    crypto::hmac_sha256_base64(signature, m_secret, std::string_view(prehash, message.out));

    const auto request = std::format_to_n(m_request, kRequestCapacity, R"({{"op":"login","args":[{{"apiKey":"{}","passphrase":"{}","timestamp":"{}","sign":"{}"}}]}})",
                                          std::string_view(m_key), std::string_view(m_passphrase), timestamp.count(), std::string_view(signature));
    if (static_cast<size_t>(request.size) > kRequestCapacity) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx websocket {} login request is too long", connection.name);
        disconnect(connection);

        return;
    }

    if (send(connection, std::string_view(m_request, request.out)))
    {
        m_log.log("okx websocket {} login sent", connection.name);
    }
}

void OkxWebsocket::keepalive(Connection &connection) noexcept
{
    if (!connection.client.connected())
    {
        return;
    }

    const nanoseconds now = time::now_monotonic();
    if (connection.pinged != nanoseconds::zero())
    {
        if (now - connection.pinged >= kPongTimeout)
        {
            m_log.log<logging::LogLevel::Error>("okx websocket {} pong timeout", connection.name);
            disconnect(connection);
        }

        return;
    }

    if (now - connection.received >= kPingInterval && send(connection, "ping"))
    {
        connection.pinged = now;
    }
}

void OkxWebsocket::receive(Connection &connection) noexcept
{
    do
    {
        std::string_view text;
        const network::WebsocketStatus status = connection.client.read(text, kReadTimeout);

        if (status == network::WebsocketStatus::Closed) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Warning>("okx websocket {} closed by server: {}", connection.name, connection.client.error());
            disconnect(connection);

            return;
        }

        if (status == network::WebsocketStatus::Failed) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Error>("okx websocket {} read failed: {}", connection.name, connection.client.error());
            disconnect(connection);

            return;
        }

        connection.received = time::now_monotonic();
        connection.pinged = nanoseconds::zero();

        if (status == network::WebsocketStatus::Message)
        {
            m_log.log<logging::LogLevel::Debug>("okx websocket {} received: {}", connection.name, text);
        }
    }
    while (connection.client.pending());

    m_log.flush();
}

bool OkxWebsocket::send(Connection &connection, std::string_view text) noexcept
{
    if (connection.client.send(text)) [[likely]]
    {
        return true;
    }

    m_log.log<logging::LogLevel::Error>("okx websocket {} send failed: {}", connection.name, connection.client.error());
    disconnect(connection);

    return false;
}

} // namespace nlx::exchange
