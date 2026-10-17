#include "exchange/okx_http_client.h"

#include <algorithm>
#include <bit>
#include <format>

namespace nlx::exchange
{

namespace
{

constexpr uint8_t slot_bit(size_t index) noexcept
{
    return static_cast<uint8_t>(1u << index);
}

} // namespace

OkxHttpClient::OkxHttpClient(const OkxConfig &config, const Credentials &credentials,
                             OkxRateLimiter &limiter, logging::LoggerRegistry &registry) :
    m_credentials(credentials),
    m_limiter(limiter),
    m_log(registry.create_node("okx_http")),
    m_running(true),
    m_lock(),
    m_busy(0),
    m_launched(0),
    m_key(),
    m_passphrase(),
    m_simulated(),
    m_public_headers(nullptr),
    m_http_client(),
    m_slots()
{
    std::format_to_n(m_key, sizeof(m_key) - 1, "{}{}", kKeyHeader, credentials.api_key);
    std::format_to_n(m_passphrase, sizeof(m_passphrase) - 1, "{}{}", kPassphraseHeader, credentials.api_passphrase);

    m_simulated[0] = { const_cast<char *>(kSimulatedHeader), nullptr };
    m_public_headers = config.simulated ? m_simulated : nullptr;

    for (Slot &slot : m_slots)
    {
        slot.handle = m_http_client.open();

        slot.headers[0] = { m_key, &slot.headers[1] };
        slot.headers[1] = { m_passphrase, &slot.headers[2] };
        slot.headers[2] = { slot.timestamp, &slot.headers[3] };
        slot.headers[3] = { slot.sign, m_public_headers };
        slot.headers[4] = { const_cast<char *>(kContentTypeHeader), m_public_headers };
    }
}

OkxHttpClient::~OkxHttpClient() noexcept
{
    for (Slot &slot : m_slots)
    {
        if (slot.handle != nullptr)
        {
            m_http_client.close(slot.handle);
        }
    }
}

void OkxHttpClient::start() noexcept
{
    m_log.log("okx http client started");

    while (m_running.load(std::memory_order_acquire))
    {
        const nanoseconds timeout = launch();
        m_http_client.perform(timeout);
        complete();
    }

    abort();

    m_log.log("okx http client stopped");
}

void OkxHttpClient::request_stop() noexcept
{
    {
        const std::lock_guard<std::mutex> lock(m_lock);
        m_running.store(false, std::memory_order_release);
    }

    m_http_client.wakeup();
}

bool OkxHttpClient::get_time(network::HttpRequest &request) noexcept
{
    return push(kTimeRoute, request, {}, {});
}

bool OkxHttpClient::get_account_config(network::HttpRequest &request) noexcept
{
    return push(kAccountConfigRoute, request, {}, {});
}

bool OkxHttpClient::get_balance(network::HttpRequest &request, std::string_view currency) noexcept
{
    char query[kTargetCapacity];
    const auto result = std::format_to_n(query, sizeof(query), "?ccy={}", currency);

    const size_t size = static_cast<size_t>(result.size);
    if (size > sizeof(query))
    {
        return false;
    }

    return push(kBalanceRoute, request, { query, size }, {});
}

bool OkxHttpClient::get_positions(network::HttpRequest &request) noexcept
{
    return push(kPositionsRoute, request, {}, {});
}

bool OkxHttpClient::set_position_mode(network::HttpRequest &request, bool hedge) noexcept
{
    const std::string_view body = hedge ? R"({"posMode":"long_short_mode"})" : R"({"posMode":"net_mode"})";
    return push(kSetPositionModeRoute, request, {}, body);
}

bool OkxHttpClient::set_leverage(network::HttpRequest &request, std::string_view instrument, uint32_t leverage,
                                 std::string_view margin_mode, std::string_view position_side) noexcept
{
    char body[kBodyCapacity];
    auto result = std::format_to_n(body, sizeof(body), R"({{"instId":"{}","lever":"{}","mgnMode":"{}"}})",
                                   instrument, leverage, margin_mode);

    if (!position_side.empty())
    {
        result = std::format_to_n(body, sizeof(body), R"({{"instId":"{}","lever":"{}","mgnMode":"{}","posSide":"{}"}})",
                                  instrument, leverage, margin_mode, position_side);
    }

    const size_t size = static_cast<size_t>(result.size);
    if (size > sizeof(body))
    {
        return false;
    }

    return push(kSetLeverageRoute, request, {}, { body, size });
}

bool OkxHttpClient::push(const Route &route, network::HttpRequest &request, std::string_view query, std::string_view body) noexcept
{
    if (route.path.size() + query.size() >= kTargetCapacity || body.size() > kBodyCapacity)
    {
        return false;
    }

    {
        const std::lock_guard<std::mutex> lock(m_lock);

        const uint8_t busy = m_busy.load(std::memory_order_acquire);
        if (!m_running.load(std::memory_order_relaxed) || busy == kSlotsMask)
        {
            return false;
        }

        const size_t index = static_cast<size_t>(std::countr_one(busy));

        Slot &slot = m_slots[index];
        slot.route = &route;
        slot.request = &request;

        const auto result = std::format_to_n(slot.url, sizeof(slot.url) - 1, "{}{}{}", kBaseUrl, route.path, query);
        *result.out = '\0';

        std::copy_n(body.data(), body.size(), slot.body);
        slot.body_size = body.size();

        m_busy.fetch_or(slot_bit(index), std::memory_order_release);
    }

    m_http_client.wakeup();

    return true;
}

nanoseconds OkxHttpClient::launch() noexcept
{
    const uint8_t queued = m_busy.load(std::memory_order_acquire) & static_cast<uint8_t>(~m_launched);
    const nanoseconds now = time::now_monotonic();

    nanoseconds timeout = nanoseconds::max();
    for (size_t index = 0; index < kSlotsSize; ++index)
    {
        if ((queued & slot_bit(index)) == 0)
        {
            continue;
        }

        Slot &slot = m_slots[index];

        const nanoseconds delay = m_limiter.acquire(slot.route->request, now);
        if (delay != nanoseconds::zero())
        {
            timeout = std::min(timeout, delay);
            continue;
        }

        const std::string_view body(slot.body, slot.body_size);

        curl_slist *headers = m_public_headers;
        if (slot.route->sign)
        {
            sign(slot);
            slot.headers[3].next = body.empty() ? m_public_headers : &slot.headers[4];
            headers = slot.headers;
        }

        slot.sent = now;
        m_launched |= slot_bit(index);
        m_http_client.send(slot.handle, slot.url, body, headers, *slot.request);
    }

    return timeout;
}

void OkxHttpClient::complete() noexcept
{
    CURL *handle = nullptr;
    while (network::HttpRequest *request = m_http_client.complete(handle))
    {
        size_t index = 0;
        while (m_slots[index].handle != handle)
        {
            ++index;
        }

        const Slot &slot = m_slots[index];
        const std::string_view target(slot.url + sizeof(kBaseUrl) - 1);
        const auto elapsed = static_cast<float>((time::now_monotonic() - slot.sent).count()) / 1000000.0f;

        if (request->code == 200)
        {
            m_log.log("request {} done: bytes = {}, time = {} ms", target, request->size, elapsed);
        }
        else
        {
            m_log.log<logging::LogLevel::Warning>("request {} failed: code = {}, time = {} ms, response = {}",
                                                  target, request->code, elapsed, std::string_view(request->buffer, request->size));
        }

        if (request->truncated)
        {
            m_log.log<logging::LogLevel::Warning>("request {} truncated: bytes = {}", target, request->size);
        }

        if (request->code == 429)
        {
            m_limiter.penalize(slot.route->request, time::now_monotonic());
        }

        release(index, *request);
    }
}

void OkxHttpClient::abort() noexcept
{
    const uint8_t busy = m_busy.load(std::memory_order_acquire);
    if (busy != 0)
    {
        m_log.log<logging::LogLevel::Warning>("{} requests aborted", std::popcount(busy));
    }

    for (size_t index = 0; index < kSlotsSize; ++index)
    {
        if ((busy & slot_bit(index)) == 0)
        {
            continue;
        }

        Slot &slot = m_slots[index];
        if ((m_launched & slot_bit(index)) != 0)
        {
            m_http_client.cancel(slot.handle);
        }

        network::HttpRequest &request = *slot.request;
        request.size = 0;
        request.code = -CURLE_ABORTED_BY_CALLBACK;
        request.truncated = false;

        release(index, request);
    }
}

void OkxHttpClient::release(size_t index, network::HttpRequest &request) noexcept
{
    const FileDescriptor &notify = request.notify;

    m_launched &= static_cast<uint8_t>(~slot_bit(index));
    m_busy.fetch_and(static_cast<uint8_t>(~slot_bit(index)), std::memory_order_release);

    nlx::detail::notify(notify);
}

void OkxHttpClient::sign(Slot &slot) noexcept
{
    const std::array<char, time::iso8601_ms_size> timestamp = nlx::detail::format_iso8601_ms(time::now());

    const std::string_view method = slot.body_size == 0 ? "GET" : "POST";
    const std::string_view target(slot.url + sizeof(kBaseUrl) - 1);
    const std::string_view body(slot.body, slot.body_size);

    char message[time::iso8601_ms_size + sizeof("POST") + kTargetCapacity + kBodyCapacity];
    const auto result = std::format_to_n(message, sizeof(message), "{}{}{}{}", timestamp.data(), method, target, body);

    char signature[crypto::hmac_sha256_base64_size];
    crypto::hmac_sha256_base64(signature, m_credentials.api_secret, { message, static_cast<size_t>(result.size) });

    std::format_to_n(slot.timestamp, sizeof(slot.timestamp) - 1, "{}{}", kTimestampHeader, timestamp.data());
    std::format_to_n(slot.sign, sizeof(slot.sign) - 1, "{}{}", kSignHeader, signature);
}

} // namespace nlx::exchange
