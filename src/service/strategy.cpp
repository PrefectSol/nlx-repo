#include "service/strategy.h"
#include "core/time.h"
#include "parsing/json_reader.h"

#include <array>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string_view>

namespace nlx::service
{

Strategy::Strategy(const OkxConfig &config, const Credentials &credentials, logging::LoggerRegistry &registry) :
    m_log(registry.create_node("strategy")),
    m_okx_rate_limiter(),
    m_okx_http_client(config, credentials, m_okx_rate_limiter, registry),
    m_okx_http_thread(),
    m_epoll(),
    m_stop(),
    m_available_balance(0.0)
{
    m_epoll = FileDescriptor::make_epoll();
    m_stop = FileDescriptor::make_eventfd(true);

    detail::epoll_add(m_epoll, m_stop);
}

Strategy::~Strategy()
{
    shutdown();
}

void Strategy::initialize()
{
    m_okx_http_thread = std::thread([this] { m_okx_http_client.start(); });

    const FileDescriptor notify = FileDescriptor::make_eventfd(true);

    char buffer[kBufferCapacity];
    network::HttpRequest request{ buffer, sizeof(buffer) - parsing::json_padding_size, 0, 0, false, notify };

    authorize(request);
    load_balance(request);
    synchronize(request);
}

void Strategy::start() noexcept
{
    m_log.log("strategy started");
    
    epoll_event event{};
    while (true)
    {
        const size_t count = detail::epoll_wait(m_epoll, { &event, 1 }, -1);
        if (count == 0)
        {
            continue;
        }

        if (event.data.fd == m_stop.get())
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

void Strategy::shutdown() noexcept
{
    if (m_okx_http_thread.joinable())
    {
        m_okx_http_client.request_stop();
        m_okx_http_thread.join();
    }
}

void Strategy::authorize(network::HttpRequest &request)
{
    if (!m_okx_http_client.get_account_config(request))
    {
        throw std::runtime_error("okx account config request rejected");
    }

    detail::wait(request.notify);

    int code = -1;
    uint64_t uid = 0;
    int mode = 0;
    bool hedge = false;
    bool trade = false;

    const bool parsed = parsing::detail::okx_parse_account_config({ request.buffer, request.size }, code, uid, mode, hedge, trade);
    if (request.code != 200 || !parsed || code != 0)
    {
        throw std::runtime_error(std::format("okx authorization failed: http = {}, code = {}", request.code, code));
    }

    m_log.log("okx uid = {}", uid);
    m_log.log("okx account mode = {}", mode);
    m_log.log("okx hedge mode = {}", hedge);
    m_log.log("okx trade permission = {}", trade);
    m_log.flush();

    if (mode != 2)
    {
        throw std::runtime_error(std::format("okx account mode {} is not futures mode", mode));
    }

    check_positions(request);

    if (hedge)
    {
        return;
    }

    if (!m_okx_http_client.set_position_mode(request, true))
    {
        throw std::runtime_error("okx position mode request rejected");
    }

    detail::wait(request.notify);

    const bool changed = parsing::detail::okx_parse_code({ request.buffer, request.size }, code);
    if (request.code != 200 || !changed || code != 0)
    {
        throw std::runtime_error(std::format("okx hedge mode failed: http = {}, code = {}", request.code, code));
    }

    m_log.log("okx hedge mode enabled");
    m_log.flush();
}

void Strategy::check_positions(network::HttpRequest &request)
{
    if (!m_okx_http_client.get_positions(request))
    {
        throw std::runtime_error("okx positions request rejected");
    }

    detail::wait(request.notify);

    int code = -1;
    size_t count = 0;

    const bool parsed = parsing::detail::okx_parse_positions({ request.buffer, request.size }, code, count);
    if (request.code != 200 || !parsed || code != 0)
    {
        throw std::runtime_error(std::format("okx positions request failed: http = {}, code = {}", request.code, code));
    }

    m_log.log("okx open positions = {}", count);
    m_log.flush();

    if (count != 0)
    {
        throw std::runtime_error(std::format("okx account has {} open positions", count));
    }
}

void Strategy::load_balance(network::HttpRequest &request)
{
    if (!m_okx_http_client.get_balance(request, "USDT"))
    {
        throw std::runtime_error("okx balance request rejected");
    }

    detail::wait(request.notify);

    int code = -1;

    const bool parsed = parsing::detail::okx_parse_balance({ request.buffer, request.size }, code, "USDT", m_available_balance);
    if (request.code != 200 || !parsed || code != 0)
    {
        throw std::runtime_error(std::format("okx balance request failed: http = {}, code = {}", request.code, code));
    }

    m_log.log("okx available balance = {} USDT", m_available_balance);
    m_log.flush();
}

void Strategy::synchronize(network::HttpRequest &request)
{
    if (!m_okx_http_client.get_time(request))
    {
        throw std::runtime_error("okx time request rejected");
    }

    detail::wait(request.notify);

    int code = -1;
    milliseconds timestamp{};

    const bool parsed = parsing::detail::okx_parse_time({ request.buffer, request.size }, code, timestamp);
    if (request.code != 200 || !parsed || code != 0)
    {
        throw std::runtime_error(std::format("okx time request failed: http = {}, code = {}", request.code, code));
    }

    const std::array<char, time::iso8601_ms_size> server = detail::format_iso8601_ms(timestamp);

    m_log.log("okx system time = {}", std::string_view(server.data(), server.size() - 1));
    m_log.flush();
}

} // namespace nlx::service
