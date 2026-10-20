#include "exchange/okx_http_client.h"
#include "core/fatal.h"
#include "core/string.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace nlx::exchange
{

OkxHttpClient::OkxHttpClient(const OkxConfig &config, const Credentials &credentials, OkxRateLimiter &limiter, logging::LoggerRegistry &registry) :
    m_log(),
    m_limiter(limiter),
    m_stop(),
    m_initialized(false),
    m_http_client(),
    m_key(),
    m_passphrase(),
    m_simulated(),
    m_secret(),
    m_slots(),
    m_busy(0),
    m_launched(0),
    m_lock()
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

    for (size_t index = 0; index < kSlotsSize; ++index)
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

bool OkxHttpClient::push(const Route &route, network::HttpRequest &request, std::string_view parameters) noexcept
{
    const size_t size = parameters.size();
    if (size > kParametersCapacity) [[unlikely]]
    {
        return false;
    }

    request.size = 0;
    request.code = 0;
    request.truncated = false;

    {
        const std::lock_guard<std::mutex> lock(m_lock);

        if (m_busy == kAllSlots)
        {
            return false;
        }

        const size_t index = static_cast<size_t>(std::countr_one(m_busy));
        Slot &slot = m_slots[index];

        slot.route = &route;
        slot.request = &request;
        slot.size = size;
        std::copy_n(parameters.data(), size, slot.parameters);

        m_busy |= static_cast<uint8_t>(1u << index);
    }

    m_http_client.wakeup();

    return true;
}

bool OkxHttpClient::get_system_time(network::HttpRequest &request) noexcept
{
    return push(kSystemTimeRoute, request, {});
}

bool OkxHttpClient::get_account_config(network::HttpRequest &request) noexcept
{
    return push(kAccountConfigRoute, request, {});
}

bool OkxHttpClient::get_balance(network::HttpRequest &request, std::string_view currency) noexcept
{
    char parameters[kParametersCapacity];
    const auto result = std::format_to_n(parameters, sizeof(parameters), "ccy={}", currency);
    if (static_cast<size_t>(result.size) > sizeof(parameters)) [[unlikely]]
    {
        return false;
    }

    return push(kBalanceRoute, request, { parameters, static_cast<size_t>(result.size) });
}

bool OkxHttpClient::get_positions(network::HttpRequest &request) noexcept
{
    return push(kPositionsRoute, request, {});
}

bool OkxHttpClient::get_pending_orders(network::HttpRequest &request, uint32_t limit) noexcept
{
    char parameters[kParametersCapacity];
    const auto result = std::format_to_n(parameters, sizeof(parameters), "limit={}", limit);
    if (static_cast<size_t>(result.size) > sizeof(parameters)) [[unlikely]]
    {
        return false;
    }

    return push(kPendingOrdersRoute, request, { parameters, static_cast<size_t>(result.size) });
}

bool OkxHttpClient::set_position_mode(network::HttpRequest &request, bool hedge) noexcept
{
    const std::string_view body = hedge ? R"({"posMode":"long_short_mode"})" : R"({"posMode":"net_mode"})";
    return push(kSetPositionModeRoute, request, body);
}

bool OkxHttpClient::set_account_mode(network::HttpRequest &request, uint32_t level) noexcept
{
    char parameters[kParametersCapacity];
    const auto result = std::format_to_n(parameters, sizeof(parameters), R"({{"acctLv":"{}"}})", level);
    if (static_cast<size_t>(result.size) > sizeof(parameters)) [[unlikely]]
    {
        return false;
    }

    return push(kSetAccountModeRoute, request, { parameters, static_cast<size_t>(result.size) });
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
        if (nlx::detail::drain(m_stop) != 0)
        {
            break;
        }
    }

    m_log.log("okx http client stopped");
    m_log.flush();
}




} // namespace nlx::exchange
