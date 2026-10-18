#include "service/strategy.h"
#include "core/fatal.h"
#include "core/time.h"
#include "parsing/json_reader.h"

#include <array>
#include <exception>
#include <format>
#include <stdexcept>
#include <string_view>

namespace nlx::service
{

Strategy::Strategy(exchange::OkxHttpClient &okx_http_client, logging::LoggerRegistry &registry) :
    m_log(registry.create_node("strategy")),
    m_okx_http_client(okx_http_client),
    m_epoll(),
    m_stop(),
    m_okx_notify(),
    m_initialized(false)
{
    m_epoll = FileDescriptor::make_epoll();
    m_stop = FileDescriptor::make_eventfd(true);
    m_okx_notify = FileDescriptor::make_eventfd(true);

    detail::epoll_add(m_epoll, m_stop);
}

bool Strategy::initialize(const FileDescriptor &interrupt)
{
    char buffer[kBufferCapacity];
    network::HttpRequest request{ buffer, sizeof(buffer) - parsing::json_padding_size, 0, 0, false, m_okx_notify };

    try
    {
        prepare(request, interrupt);
    }
    catch (const std::exception &exception)
    {
        if (request.code == -CURLE_ABORTED_BY_CALLBACK)
        {
            m_log.log<logging::LogLevel::Warning>("strategy initialization interrupted");
            m_log.flush();
            return false;
        }

        m_log.log<logging::LogLevel::Error>("strategy initialization failed: {}", exception.what());
        m_log.flush();
        throw;
    }

    m_initialized.store(true, std::memory_order_release);

    return true;
}

void Strategy::start() noexcept
{
    if (!m_initialized.load(std::memory_order_acquire)) [[unlikely]]
    {
        NLX_FATAL("nlx::service::Strategy::start() called before initialize()");
    }

    m_log.log("strategy started");

    char buffer[kBufferCapacity];
    network::HttpRequest request{ buffer, sizeof(buffer) - parsing::json_padding_size, 0, 0, false, m_okx_notify };

    int code = -1;
    double balance = 0.0;
    if (okx_get_balance(request, m_stop, code, balance))
    {
        m_log.log("okx available balance = {} USDT", balance);
    }
    else if (request.code == -CURLE_ABORTED_BY_CALLBACK)
    {
        m_log.log<logging::LogLevel::Warning>("okx balance request interrupted");
    }
    else
    {
        m_log.log<logging::LogLevel::Error>("okx balance request failed: http = {}, code = {}", request.code, code);
    }

    m_log.flush();

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

void Strategy::prepare(network::HttpRequest &request, const FileDescriptor &interrupt)
{
    int code = -1;
    uint64_t uid = 0;
    int mode = 0;
    bool hedge = false;
    bool trade = false;

    if (!okx_get_account_config(request, interrupt, code, uid, mode, hedge, trade))
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

    size_t positions = 0;
    if (!okx_get_positions(request, interrupt, code, positions))
    {
        throw std::runtime_error(std::format("okx positions request failed: http = {}, code = {}", request.code, code));
    }

    m_log.log("okx open positions = {}", positions);
    m_log.flush();

    if (positions != 0)
    {
        throw std::runtime_error(std::format("okx account has {} open positions", positions));
    }

    bool orders = false;
    if (!okx_get_orders(request, interrupt, code, orders))
    {
        throw std::runtime_error(std::format("okx orders request failed: http = {}, code = {}", request.code, code));
    }

    m_log.log("okx open orders = {}", orders);
    m_log.flush();

    if (orders)
    {
        throw std::runtime_error("okx account has open orders");
    }

    if (!hedge)
    {
        if (!okx_set_hedge_mode(request, interrupt, code))
        {
            throw std::runtime_error(std::format("okx hedge mode failed: http = {}, code = {}", request.code, code));
        }

        m_log.log("okx hedge mode enabled");
        m_log.flush();
    }

    milliseconds timestamp{};
    if (!okx_get_system_time(request, interrupt, code, timestamp))
    {
        throw std::runtime_error(std::format("okx time request failed: http = {}, code = {}", request.code, code));
    }

    const std::array<char, time::iso8601_ms_size> server = detail::format_iso8601_ms(timestamp);

    m_log.log("okx system time = {}", std::string_view(server.data(), server.size() - 1));
    m_log.flush();
}

bool Strategy::okx_get_account_config(network::HttpRequest &request, const FileDescriptor &interrupt,
                                      int &code, uint64_t &uid, int &mode, bool &hedge, bool &trade) noexcept
{
    code = -1;

    return okx_await(m_okx_http_client.get_account_config(request), request, interrupt) &&
           parsing::detail::okx_parse_account_config({ request.buffer, request.size }, code, uid, mode, hedge, trade) &&
           request.code == 200 &&
           code == 0;
}

bool Strategy::okx_get_positions(network::HttpRequest &request, const FileDescriptor &interrupt, int &code, size_t &count) noexcept
{
    code = -1;

    return okx_await(m_okx_http_client.get_positions(request), request, interrupt) &&
           parsing::detail::okx_parse_positions({ request.buffer, request.size }, code, count) &&
           request.code == 200 &&
           code == 0;
}

bool Strategy::okx_get_orders(network::HttpRequest &request, const FileDescriptor &interrupt, int &code, bool &open) noexcept
{
    code = -1;

    return okx_await(m_okx_http_client.get_pending_orders(request, 1), request, interrupt) &&
           parsing::detail::okx_parse_orders({ request.buffer, request.size }, code, open) &&
           request.code == 200 &&
           code == 0;
}

bool Strategy::okx_set_hedge_mode(network::HttpRequest &request, const FileDescriptor &interrupt, int &code) noexcept
{
    code = -1;

    return okx_await(m_okx_http_client.set_position_mode(request, true), request, interrupt) &&
           parsing::detail::okx_parse_code({ request.buffer, request.size }, code) &&
           request.code == 200 &&
           code == 0;
}

bool Strategy::okx_get_balance(network::HttpRequest &request, const FileDescriptor &interrupt, int &code, double &available) noexcept
{
    code = -1;

    return okx_await(m_okx_http_client.get_balance(request, "USDT"), request, interrupt) &&
           parsing::detail::okx_parse_balance({ request.buffer, request.size }, code, "USDT", available) &&
           request.code == 200 &&
           code == 0;
}

bool Strategy::okx_get_system_time(network::HttpRequest &request, const FileDescriptor &interrupt, int &code, milliseconds &timestamp) noexcept
{
    code = -1;

    return okx_await(m_okx_http_client.get_time(request), request, interrupt) &&
           parsing::detail::okx_parse_time({ request.buffer, request.size }, code, timestamp) &&
           request.code == 200 &&
           code == 0;
}

bool Strategy::okx_await(bool queued, network::HttpRequest &request, const FileDescriptor &interrupt) noexcept
{
    if (!queued)
    {
        request.size = 0;
        request.code = 0;
        return false;
    }

    if (detail::wait(request.notify, interrupt))
    {
        return true;
    }

    m_okx_http_client.request_stop();
    detail::wait(request.notify);

    request.size = 0;
    request.code = -CURLE_ABORTED_BY_CALLBACK;

    return false;
}

} // namespace nlx::service
