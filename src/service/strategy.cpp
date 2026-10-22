#include "service/strategy.h"
#include "core/fatal.h"
#include "core/time.h"
#include "core/cpu.h"
#include "parsing/json_reader.h"

#include <array>
#include <format>
#include <stdexcept>
#include <string_view>

namespace nlx::service
{

Strategy::Strategy(exchange::OkxHttpClient &okx_http_client, logging::LoggerRegistry &registry) :
    m_log(),
    m_okx_http_client(okx_http_client),
    m_epoll(),
    m_stop(),
    m_okx_notify(),
    m_initialized(false)
{
    m_log = registry.create_node("strategy");

    m_epoll = FileDescriptor::make_epoll();
    m_stop = FileDescriptor::make_eventfd(true);
    m_okx_notify = FileDescriptor::make_eventfd(true);

    detail::epoll_add(m_epoll, m_stop);
}

void Strategy::initialize()
{
    constexpr size_t buffer_capacity = 256 * 1024;
    static_assert(buffer_capacity > parsing::json_padding_size);
    
    char buffer[buffer_capacity];
    network::HttpRequest request{ buffer, sizeof(buffer) - parsing::json_padding_size, 0, 0, false, m_okx_notify };

    int code = -1;

    milliseconds timestamp{};
    bool result = okx_get_system_time(request, code, timestamp);
    if (!result)
    {
        throw std::runtime_error(std::format("okx time request failed: http = {}, code = {}", request.code, code));
    }

    const std::array<char, time::iso8601_ms_size> servertime = detail::format_iso8601_ms(timestamp);
    m_log.log("okx system time = {}", std::string_view(servertime.data(), servertime.size() - 1));
    m_log.flush();

    uint64_t uid = 0;
    int mode = 0;
    bool hedge = false;
    bool trade = false;
    result = okx_get_account_config(request, code, uid, mode, hedge, trade);
    if (!result)
    {
        throw std::runtime_error(std::format("okx authorization failed: http = {}, code = {}", request.code, code));
    }

    m_log.log("okx uid = {}", uid);
    m_log.log("okx account mode = {}", mode);
    m_log.log("okx hedge mode = {}", hedge);
    m_log.log("okx trade permission = {}", trade);
    m_log.flush();

    size_t positions = 0;
    result = okx_get_positions(request, code, positions);
    if (!result)
    {
        throw std::runtime_error(std::format("okx positions request failed: http = {}, code = {}", request.code, code));
    }

    m_log.log("okx open positions = {}", positions);
    m_log.flush();

    if (positions != 0)
    {
        throw std::runtime_error(std::format("okx account has {} open positions", positions));
    }

    size_t orders = 0;
    result = okx_get_orders(request, code, orders);
    if (!result)
    {
        throw std::runtime_error(std::format("okx orders request failed: http = {}, code = {}", request.code, code));
    }

    m_log.log("okx open orders = {}", orders);
    m_log.flush();

    if (orders != 0)
    {
        throw std::runtime_error(std::format("okx account has {} open orders", orders));
    }

    if (mode != 2)
    {
        result = okx_set_futures_mode(request, code);
        if (!result)
        {
            throw std::runtime_error(std::format("okx set account futures mode failed: http = {}, code = {}", request.code, code));
        }

        m_log.log("okx account futures mode enabled");
        m_log.flush();
    }

    if (!hedge)
    {
        result = okx_set_hedge_mode(request, code);
        if (!result)
        {
            throw std::runtime_error(std::format("okx set hedge mode failed: http = {}, code = {}", request.code, code));
        }

        m_log.log("okx account hedge mode enabled");
        m_log.flush();
    }

    m_initialized.store(true, std::memory_order_release);
}

void Strategy::start() noexcept
{
    if (!m_initialized.load(std::memory_order_acquire)) [[unlikely]]
    {
        NLX_FATAL("nlx::service::Strategy::start() called before initialize()");
    }

    const bool success = cpu::set_name(cpu::this_native_handle(), "nlx-strategy");
    if (!success) [[unlikely]]
    {
        NLX_FATAL("nlx::service::Strategy::start() cpu::set_name failed");
    }

    m_log.log("strategy started");
    m_log.flush();

    epoll_event event{};
    while (true)
    {
        const size_t count = detail::epoll_wait(m_epoll, { &event, 1 }, -1);
        if (count == 0)
        {
            continue;
        }

        if (event.data.fd == m_stop)
        {
            [[maybe_unused]] const uint64_t result = detail::drain(m_stop);
            break;
        }
    }

    m_log.log("strategy stopped");
    m_log.flush();
}

void Strategy::request_stop() noexcept
{
    detail::notify(m_stop);
}

bool Strategy::okx_get_system_time(network::HttpRequest &request, int &code, milliseconds &timestamp) noexcept
{
    code = -1;
    bool result = m_okx_http_client.get_system_time(request);
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_time({ request.buffer, request.size }, code, timestamp);

    return result && request.code == 200 && code == 0;
}

bool Strategy::okx_get_account_config(network::HttpRequest &request, int &code, uint64_t &uid, int &mode, bool &hedge, bool &trade) noexcept
{
    code = -1;
    bool result = m_okx_http_client.get_account_config(request);
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_account_config({ request.buffer, request.size }, code, uid, mode, hedge, trade);

    return result && request.code == 200 && code == 0;
}

bool Strategy::okx_get_positions(network::HttpRequest &request, int &code, size_t &count) noexcept
{
    code = -1;
    bool result = m_okx_http_client.get_positions(request);
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_positions({ request.buffer, request.size }, code, count);

    return result && request.code == 200 && code == 0;
}

bool Strategy::okx_get_orders(network::HttpRequest &request, int &code, size_t &count) noexcept
{
    code = -1;
    bool result = m_okx_http_client.get_pending_orders(request, 100);
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_orders({ request.buffer, request.size }, code, count);

    return result && request.code == 200 && code == 0;
}

bool Strategy::okx_get_balance_usdt(network::HttpRequest &request, int &code, double &available) noexcept
{
    code = -1;
    bool result = m_okx_http_client.get_balance(request, "USDT");
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_balance({ request.buffer, request.size }, code, available, "USDT");

    return result && request.code == 200 && code == 0;
}

bool Strategy::okx_set_hedge_mode(network::HttpRequest &request, int &code) noexcept
{
    code = -1;
    bool result = m_okx_http_client.set_position_mode(request, true);
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_code({ request.buffer, request.size }, code);

    return result && request.code == 200 && code == 0;
}

bool Strategy::okx_set_futures_mode(network::HttpRequest &request, int &code) noexcept
{
    code = -1;
    bool result = m_okx_http_client.set_account_mode(request, 2);
    if (!result)
    {
        return false;
    }

    detail::wait(request.notify);
    result = parsing::detail::okx_parse_code({ request.buffer, request.size }, code);

    return result && request.code == 200 && code == 0;
}

} // namespace nlx::service
