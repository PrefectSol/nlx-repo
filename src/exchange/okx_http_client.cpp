#include "exchange/okx_http_client.h"
#include "core/crypto.h"
#include "core/fatal.h"
#include "core/string.h"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <thread>
#include <utility>

namespace nlx::exchange
{

namespace
{

using SlotMask = network::HttpClient::SlotMask;

static_assert(std::atomic<SlotMask>::is_always_lock_free);

constexpr SlotMask bit(size_t index) noexcept
{
    return static_cast<SlotMask>(SlotMask{ 1 } << index);
}

constexpr size_t lowest(SlotMask mask) noexcept
{
    return static_cast<size_t>(std::countr_zero(mask));
}

constexpr std::string_view method(bool post) noexcept
{
    return post ? "POST" : "GET";
}

constexpr double ms(nanoseconds value) noexcept
{
    return static_cast<double>(value.count()) / 1000000.0;
}

} // namespace

OkxHttpClient::OkxHttpClient(const OkxConfig &config, const Credentials &credentials, OkxRateLimiter &limiter, logging::LoggerRegistry &registry) :
    m_log(),
    m_limiter(limiter),
    m_stop(),
    m_initialized(false),
    m_stopped(false),
    m_http_client(),
    m_key(),
    m_passphrase(),
    m_simulated(),
    m_secret(),
    m_slots(),
    m_busy(0),
    m_ready(0),
    m_launched(0),
    m_frozen(0),
    m_thaw(nanoseconds::max())
{
    m_log = registry.create_node("okx_http");

    const auto key = std::format_to_n(m_key, sizeof(m_key) - 1, "OK-ACCESS-KEY: {}", credentials.api_key);
    if (static_cast<size_t>(key.size) >= kHeaderCapacity) [[unlikely]]
    {
        throw std::invalid_argument("okx header api key is too long");
    }

    const auto passphrase = std::format_to_n(m_passphrase, sizeof(m_passphrase) - 1, "OK-ACCESS-PASSPHRASE: {}", credentials.api_passphrase);
    if (static_cast<size_t>(passphrase.size) >= kHeaderCapacity) [[unlikely]]
    {
        throw std::invalid_argument("okx header api passphrase is too long");
    }

    const auto simulated = std::format_to_n(m_simulated, sizeof(m_simulated) - 1, "x-simulated-trading: {}", static_cast<int>(config.simulated));
    if (static_cast<size_t>(simulated.size) >= kHeaderCapacity) [[unlikely]]
    {
        throw std::invalid_argument("okx header simulated is too long");
    }

    nlx::detail::str_assign(m_secret, credentials.api_secret);

    m_stop = FileDescriptor::make_eventfd(true);
}

void OkxHttpClient::initialize()
{
    m_http_client.initialize();

    for (size_t index = 0; index < m_slots.size(); ++index)
    {
        Slot &slot = m_slots[index];

        const char *headers[] = { slot.timestamp, slot.signature, m_key, m_passphrase, "Content-Type: application/json", m_simulated };
        m_http_client.set_headers(index, headers);
    }

    m_initialized.store(true, std::memory_order_release);
}

void OkxHttpClient::request_stop() noexcept
{
    nlx::detail::notify(m_stop);
}

template <typename ...Args>
bool OkxHttpClient::push(const Route &route, network::HttpRequest &request, std::format_string<Args...> format, Args &&...args) noexcept
{
    request.size = 0;
    request.code = 0;
    request.truncated = false;

    size_t index = 0;
    if (!claim(index)) [[unlikely]]
    {
        return false;
    }

    Slot &slot = m_slots[index];

    char *const data = slot.parameters;
    char *const end = data + kParametersCapacity;
    char *const begin = data + time::iso8601_ms_size - 1;

    const auto head = std::format_to_n(begin, end - begin, "{}{}", method(route.post), route.path);
    if (head.size > end - begin) [[unlikely]]
    {
        vacate(index);
        return false;
    }

    const auto tail = std::format_to_n(head.out, end - head.out, format, std::forward<Args>(args)...);
    if (tail.size > end - head.out) [[unlikely]]
    {
        vacate(index);
        return false;
    }

    slot.route = &route;
    slot.request = &request;
    slot.size = static_cast<size_t>(tail.out - data);

    m_ready.fetch_or(bit(index), std::memory_order_release);
    m_http_client.wakeup();

    return true;
}

bool OkxHttpClient::claim(size_t &index) noexcept
{
    SlotMask busy = m_busy.load(std::memory_order_relaxed);
    do
    {
        index = static_cast<size_t>(std::countr_one(busy));
        if (index == network::HttpClient::kTransfersSize) [[unlikely]]
        {
            return false;
        }
    }
    while (!m_busy.compare_exchange_weak(busy, static_cast<SlotMask>(busy | bit(index)), std::memory_order_seq_cst, std::memory_order_relaxed));

    if (m_stopped.load(std::memory_order_seq_cst)) [[unlikely]]
    {
        vacate(index);
        return false;
    }

    return true;
}

void OkxHttpClient::vacate(size_t index) noexcept
{
    m_ready.fetch_and(static_cast<SlotMask>(~bit(index)), std::memory_order_relaxed);
    m_busy.fetch_and(static_cast<SlotMask>(~bit(index)), std::memory_order_release);
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
    return push(kBalanceRoute, request, "?ccy={}", currency);
}

bool OkxHttpClient::get_positions(network::HttpRequest &request) noexcept
{
    return push(kPositionsRoute, request, "");
}

bool OkxHttpClient::get_pending_orders(network::HttpRequest &request, uint32_t limit) noexcept
{
    return push(kPendingOrdersRoute, request, "?limit={}", limit);
}

bool OkxHttpClient::set_position_mode(network::HttpRequest &request, bool hedge) noexcept
{
    const std::string_view body = hedge ? R"({"posMode":"long_short_mode"})" : R"({"posMode":"net_mode"})";
    return push(kSetPositionModeRoute, request, "{}", body);
}

bool OkxHttpClient::set_account_mode(network::HttpRequest &request, uint32_t level) noexcept
{
    return push(kSetAccountModeRoute, request, R"({{"acctLv":"{}"}})", level);
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
        const milliseconds timeout = schedule();

        m_http_client.perform();
        complete();

        if (m_http_client.poll(timeout, m_stop))
        {
            break;
        }
    }

    release();

    m_log.log("okx http client stopped");
    m_log.flush();
}

milliseconds OkxHttpClient::schedule() noexcept
{
    SlotMask pending = static_cast<SlotMask>(m_ready.load(std::memory_order_acquire) & ~m_launched);

    const nanoseconds now = time::now_monotonic();
    if (now >= m_thaw)
    {
        m_frozen = 0;
        m_thaw = nanoseconds::max();
    }

    for (pending &= static_cast<SlotMask>(~m_frozen); pending != 0; pending &= static_cast<SlotMask>(pending - 1))
    {
        const size_t index = lowest(pending);
        const Route &route = *m_slots[index].route;

        const nanoseconds delay = m_limiter.acquire(route.request, now);
        if (delay != nanoseconds::zero())
        {
            m_frozen |= bit(index);
            m_thaw = std::min(m_thaw, now + delay);

            m_log.log("okx http {} throttled for {} ms", route.path, ms(delay));
            continue;
        }

        m_launched |= bit(index);
        dispatch(index);
    }

    return m_frozen != 0 ? nlx::detail::to_milliseconds(m_thaw - now) : milliseconds(1000);
}

void OkxHttpClient::dispatch(size_t index) noexcept
{
    static_assert(kBaseUrl.size() < time::iso8601_ms_size);

    Slot &slot = m_slots[index];
    const Route &route = *slot.route;

    const auto timestamp = nlx::detail::format_iso8601_ms(time::now());
    const std::string_view stamp(timestamp.data(), timestamp.size() - 1);
    std::copy(stamp.begin(), stamp.end(), slot.parameters);

    char signature[crypto::hmac_sha256_base64_size];
    crypto::hmac_sha256_base64(signature, m_secret, std::string_view(slot.parameters, slot.size));

    const auto timestamp_header = std::format_to_n(slot.timestamp, kHeaderCapacity - 1, "OK-ACCESS-TIMESTAMP: {}", stamp);
    *timestamp_header.out = '\0';

    const auto signature_header = std::format_to_n(slot.signature, kHeaderCapacity - 1, "OK-ACCESS-SIGN: {}", std::string_view(signature));
    *signature_header.out = '\0';

    const char *const target = slot.parameters + stamp.size() + method(route.post).size();
    const char *const end = slot.parameters + slot.size;

    const std::string_view path = route.post ? route.path : std::string_view(target, end);
    const std::string_view body = route.post ? std::string_view(target + route.path.size(), end) : std::string_view();

    char url[kParametersCapacity];
    const auto address = std::format_to_n(url, kParametersCapacity - 1, "{}{}", kBaseUrl, path);
    *address.out = '\0';

    m_http_client.launch(index, route.post, url, body, *slot.request);

    m_log.log("okx http {} {} sent", method(route.post), path);
}

void OkxHttpClient::complete() noexcept
{
    size_t index = 0;
    network::HttpResult result{};

    while (m_http_client.read(index, result))
    {
        const Route &route = *m_slots[index].route;
        network::HttpRequest &request = *m_slots[index].request;

        if (request.code == -1) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Error>("okx http {} failed in {} ms: curl = {}, http = {}, {}", route.path, ms(result.elapsed), result.error, result.status, result.message);
            m_log.log<logging::LogLevel::Error>("okx http {} stages: sent = {}, resolve = {} ms, connect = {} ms, handshake = {} ms", route.path, result.sent, ms(result.resolve), ms(result.connect), ms(result.handshake));
        }
        else
        {
            m_log.log("okx http {} {} in {} ms", route.path, request.code, ms(result.elapsed));

            if (request.truncated) [[unlikely]]
            {
                m_log.log<logging::LogLevel::Warning>("okx http {} response truncated at {} bytes", route.path, request.capacity);
            }

            if (request.code >= 400) [[unlikely]]
            {
                m_log.log<logging::LogLevel::Warning>("okx http {} response: {}", route.path, std::string_view(request.buffer, request.size));
            }

            if (request.code == 429) [[unlikely]]
            {
                m_limiter.penalize(route.request, time::now_monotonic());
                m_log.log<logging::LogLevel::Warning>("okx http {} rejected by exchange rate limit", route.path);
            }
        }

        m_launched &= static_cast<SlotMask>(~bit(index));
        vacate(index);

        nlx::detail::notify(request.notify);
    }
}

void OkxHttpClient::release() noexcept
{
    m_stopped.store(true, std::memory_order_seq_cst);

    m_http_client.release();

    SlotMask ready = 0;
    while (true)
    {
        const SlotMask busy = m_busy.load(std::memory_order_seq_cst);
        ready = m_ready.load(std::memory_order_acquire);

        if ((busy & ~ready) == 0)
        {
            break;
        }

        std::this_thread::yield();
    }

    for (; ready != 0; ready &= static_cast<SlotMask>(ready - 1))
    {
        const size_t index = lowest(ready);
        const Slot &slot = m_slots[index];
        network::HttpRequest &request = *slot.request;

        m_log.log("okx http {} cancelled", slot.route->path);

        request.code = -1;

        vacate(index);
        nlx::detail::notify(request.notify);
    }

    m_launched = 0;
    m_frozen = 0;
    m_thaw = nanoseconds::max();

    static_cast<void>(nlx::detail::drain(m_stop));
}

} // namespace nlx::exchange
