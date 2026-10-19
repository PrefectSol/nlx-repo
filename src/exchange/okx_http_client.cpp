#include "exchange/okx_http_client.h"
#include "core/fatal.h"
#include "core/string.h"

#include <algorithm>
#include <utility>

namespace nlx::exchange
{

template<typename ...Args>
bool OkxHttpClient::push(const Route &route, network::HttpRequest &request, std::format_string<Args...> format, Args &&...args) noexcept
{
    request.size = 0;
    request.code = 0;

    bool queued = false;
    {
        const std::lock_guard<std::mutex> lock(m_lock);

        size_t index = 0;
        while (index < kSlotsSize && m_slots[index].state != SlotState::Free)
        {
            ++index;
        }

        if (index == kSlotsSize)
        {
            ++m_rejected;
        }
        else
        {
            Slot &slot = m_slots[index];

            const auto result = std::format_to_n(slot.parameters, kParametersCapacity, format, std::forward<Args>(args)...);
            if (static_cast<size_t>(result.size) > kParametersCapacity) [[unlikely]]
            {
                NLX_FATAL("nlx::exchange::OkxHttpClient::push() parameters exceed capacity");
            }

            slot.state = SlotState::Queued;
            slot.route = &route;
            slot.request = &request;
            slot.size = static_cast<size_t>(result.size);

            queued = true;
        }
    }

    m_http_client.wakeup();

    return queued;
}

OkxHttpClient::OkxHttpClient(const OkxConfig &config, const Credentials &credentials, OkxRateLimiter &limiter, logging::LoggerRegistry &registry) :
    m_log(),
    m_limiter(limiter),
    m_key(),
    m_passphrase(),
    m_secret(),
    m_simulated(config.simulated),
    m_lock(),
    m_slots(),
    m_rejected(0),
    m_stop(),
    m_initialized(false),
    m_http_client()
{
    m_log = registry.create_node("okx_http");

    std::format_to_n(m_key, sizeof(m_key) - 1, "{}{}", kKeyHeader, credentials.api_key);
    std::format_to_n(m_passphrase, sizeof(m_passphrase) - 1, "{}{}", kPassphraseHeader, credentials.api_passphrase);
    nlx::detail::str_assign(m_secret, credentials.api_secret);

    m_stop = FileDescriptor::make_eventfd(true);
}

void OkxHttpClient::initialize()
{
    m_http_client.initialize();
    m_initialized.store(true, std::memory_order_release);
}

void OkxHttpClient::start() noexcept
{
    if (!m_initialized.load(std::memory_order_acquire)) [[unlikely]]
    {
        NLX_FATAL("nlx::exchange::OkxHttpClient::start() called before initialize()");
    }

    m_log.log("okx http client started");
    m_log.flush();

    while (true)
    {
        const nanoseconds timeout = launch();
        m_http_client.perform(m_stop, timeout);

        if (nlx::detail::drain(m_stop) != 0)
        {
            break;
        }

        complete();
    }

    abort_requests();

    m_log.log("okx http client stopped");
    m_log.flush();
}

void OkxHttpClient::request_stop() noexcept
{
    nlx::detail::notify(m_stop);
}

bool OkxHttpClient::get_system_time(network::HttpRequest &request) noexcept
{
    return push(kSystemTimeRoute, request, "");
}

bool OkxHttpClient::get_account_config(network::HttpRequest &request) noexcept
{
    return push(kAccountConfigRoute, request, "");
}

bool OkxHttpClient::get_balance(network::HttpRequest &request, std::string_view currency) noexcept
{
    return push(kBalanceRoute, request, "ccy={}", currency);
}

bool OkxHttpClient::get_positions(network::HttpRequest &request) noexcept
{
    return push(kPositionsRoute, request, "");
}

bool OkxHttpClient::get_pending_orders(network::HttpRequest &request, uint32_t limit) noexcept
{
    return push(kPendingOrdersRoute, request, "limit={}", limit);
}

bool OkxHttpClient::set_position_mode(network::HttpRequest &request, bool hedge) noexcept
{
    return push(kSetPositionModeRoute, request, R"({{"posMode":"{}"}})", hedge ? "long_short_mode" : "net_mode");
}

bool OkxHttpClient::set_leverage(network::HttpRequest &request, std::string_view instrument, uint32_t leverage,
                                 std::string_view margin_mode, std::string_view position_side) noexcept
{
    if (position_side.empty())
    {
        return push(kSetLeverageRoute, request, R"({{"instId":"{}","lever":"{}","mgnMode":"{}"}})",
                    instrument, leverage, margin_mode);
    }

    return push(kSetLeverageRoute, request, R"({{"instId":"{}","lever":"{}","mgnMode":"{}","posSide":"{}"}})",
                instrument, leverage, margin_mode, position_side);
}

nanoseconds OkxHttpClient::launch() noexcept
{
    std::array<bool, kSlotsSize> queued{};
    uint32_t rejected = 0;
    {
        const std::lock_guard<std::mutex> lock(m_lock);

        for (size_t index = 0; index < kSlotsSize; ++index)
        {
            queued[index] = m_slots[index].state == SlotState::Queued;
        }

        rejected = std::exchange(m_rejected, 0);
    }

    if (rejected != 0)
    {
        m_log.log<logging::LogLevel::Warning>("{} requests rejected: all {} slots are busy", rejected, kSlotsSize);
    }

    const nanoseconds now = time::now_monotonic();

    nanoseconds timeout = nanoseconds::max();
    for (size_t index = 0; index < kSlotsSize; ++index)
    {
        if (!queued[index])
        {
            continue;
        }

        Slot &slot = m_slots[index];

        const nanoseconds delay = m_limiter.acquire(slot.route->request, now, slot.ticket);
        if (delay != nanoseconds::zero())
        {
            if (!slot.throttled)
            {
                slot.throttled = true;
                m_log.log<logging::LogLevel::Warning>("request {} throttled by rate limit", slot.route->path);
            }

            timeout = std::min(timeout, delay);
            continue;
        }

        send(index, now);
    }

    return timeout;
}

void OkxHttpClient::send(size_t index, nanoseconds now) noexcept
{
    Slot &slot = m_slots[index];
    const Route &route = *slot.route;

    const bool post = route.method == network::HttpMethod::Post;
    const std::string_view parameters(slot.parameters, slot.size);
    const std::string_view method = post ? "POST" : "GET";
    const std::string_view separator = post || parameters.empty() ? "" : "?";
    const std::string_view query = post ? std::string_view() : parameters;
    const std::string_view body = post ? parameters : std::string_view();

    const std::array<char, time::iso8601_ms_size> timestamp = nlx::detail::format_iso8601_ms(time::now());
    const std::string_view stamp(timestamp.data(), timestamp.size() - 1);

    char message[kLineCapacity];
    const auto prehash = std::format_to_n(message, sizeof(message), "{}{}{}{}{}{}", stamp, method, route.path, separator, query, body);
    if (static_cast<size_t>(prehash.size) > sizeof(message)) [[unlikely]]
    {
        NLX_FATAL("nlx::exchange::OkxHttpClient::send() signature message exceeds capacity");
    }

    char signature[crypto::hmac_sha256_base64_size];
    crypto::hmac_sha256_base64(signature, m_secret, { message, static_cast<size_t>(prehash.size) });

    const auto timestamp_header = std::format_to_n(slot.timestamp, sizeof(slot.timestamp) - 1, "{}{}", kTimestampHeader, stamp);
    *timestamp_header.out = '\0';

    const auto signature_header = std::format_to_n(slot.signature, sizeof(slot.signature) - 1, "{}{}", kSignatureHeader, signature);
    *signature_header.out = '\0';

    char url[kLineCapacity];
    const auto location = std::format_to_n(url, sizeof(url) - 1, "{}{}{}{}", kBaseUrl, route.path, separator, query);
    if (static_cast<size_t>(location.size) > sizeof(url) - 1) [[unlikely]]
    {
        NLX_FATAL("nlx::exchange::OkxHttpClient::send() url exceeds capacity");
    }

    *location.out = '\0';

    std::array<const char *, network::HttpClient::kHeadersSize> headers{};
    size_t count = 0;

    headers[count++] = m_key;
    headers[count++] = m_passphrase;
    headers[count++] = slot.timestamp;
    headers[count++] = slot.signature;

    if (post)
    {
        headers[count++] = kContentTypeHeader;
    }

    if (m_simulated)
    {
        headers[count++] = kSimulatedHeader;
    }

    {
        const std::lock_guard<std::mutex> lock(m_lock);
        slot.state = SlotState::Running;
    }

    slot.sent = now;
    m_http_client.send(index, route.method, url, body, { headers.data(), count }, *slot.request);
}

void OkxHttpClient::complete() noexcept
{
    size_t index = 0;
    while (m_http_client.complete(index))
    {
        Slot &slot = m_slots[index];
        const network::HttpRequest &request = *slot.request;

        const nanoseconds now = time::now_monotonic();
        const float elapsed = static_cast<float>((now - slot.sent).count()) / 1000000.0f;

        m_limiter.complete(slot.ticket, now);

        if (request.code == 200)
        {
            m_log.log("request {} done: bytes = {}, time = {} ms", slot.route->path, request.size, elapsed);
        }
        else if (request.code == network::HttpRequest::kOverflow)
        {
            m_log.log<logging::LogLevel::Warning>("request {} failed: response exceeds {} bytes", slot.route->path, request.capacity);
        }
        else
        {
            m_log.log<logging::LogLevel::Warning>("request {} failed: code = {}, time = {} ms, response = {}",
                                                  slot.route->path, request.code, elapsed, std::string_view(request.buffer, request.size));
        }

        if (request.code == 429)
        {
            m_limiter.penalize(slot.route->request, now);
            m_log.log<logging::LogLevel::Warning>("request {} rate limited by okx", slot.route->path);
        }

        release(slot);
    }
}

void OkxHttpClient::abort_requests() noexcept
{
    const nanoseconds now = time::now_monotonic();

    size_t aborted = 0;
    for (size_t index = 0; index < kSlotsSize; ++index)
    {
        Slot &slot = m_slots[index];

        SlotState state = SlotState::Free;
        {
            const std::lock_guard<std::mutex> lock(m_lock);
            state = slot.state;
        }

        if (state == SlotState::Free)
        {
            continue;
        }

        if (state == SlotState::Running)
        {
            m_http_client.cancel(index);
            m_limiter.complete(slot.ticket, now);
        }

        slot.request->size = 0;
        slot.request->code = network::HttpRequest::kAborted;

        release(slot);
        ++aborted;
    }

    if (aborted != 0)
    {
        m_log.log<logging::LogLevel::Warning>("{} requests aborted", aborted);
    }
}

void OkxHttpClient::release(Slot &slot) noexcept
{
    const FileDescriptor &notify = slot.request->notify;

    {
        const std::lock_guard<std::mutex> lock(m_lock);
        slot.state = SlotState::Free;
        slot.throttled = false;
    }

    nlx::detail::notify(notify);
}

} // namespace nlx::exchange
