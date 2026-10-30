#include "network/websocket_client.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace nlx::network
{

namespace detail
{

WebsocketLogger::WebsocketLogger() noexcept :
    m_log(nullptr)
{

}

void WebsocketLogger::attach(logging::LoggerNode &log) noexcept
{
    m_log = &log;
}

} // namespace nlx::network::detail

WebsocketClient::WebsocketClient() noexcept :
    m_logger(),
    m_context(&m_logger),
    m_buffer(),
    m_client(),
    m_fd(-1),
    m_close_code(ws_client::close_code::not_set),
    m_output(),
    m_error()
{

}

WebsocketClient::~WebsocketClient() noexcept
{
    close();
}

void WebsocketClient::initialize(logging::LoggerNode &log)
{
    if (m_buffer.has_value())
    {
        return;
    }

    m_logger.attach(log);

    const auto context = m_context.init();
    if (!context.has_value())
    {
        throw std::runtime_error(std::format("websocket ssl context init failed: {}", context.error().message));
    }

    const auto certificates = m_context.load_verify_file(CA_CERT_PATH);
    if (!certificates.has_value())
    {
        throw std::runtime_error(std::format("websocket ssl ca load failed: {}", certificates.error().message));
    }

    auto buffer = ws_client::Buffer::create(kReadCapacity, kReadCapacity);
    if (!buffer.has_value())
    {
        throw std::runtime_error(std::format("websocket buffer allocation failed: {}", buffer.error().message));
    }

    m_buffer.emplace(std::move(*buffer));
}

bool WebsocketClient::connect(std::string_view url, milliseconds timeout) noexcept
{
    close();

    if (!m_buffer.has_value()) [[unlikely]]
    {
        fail("websocket client is not initialized");
        return false;
    }

    const auto address = ws_client::URL::parse(url);
    if (!address.has_value())
    {
        fail(address.error());
        return false;
    }

    ws_client::DnsResolver<Logger> resolver(&m_logger);

    auto resolved = resolver.resolve(address->host(), address->port_str());
    if (!resolved.has_value())
    {
        fail(resolved.error());
        return false;
    }

    if (resolved->empty())
    {
        fail("websocket host has no addresses");
        return false;
    }

    for (ws_client::AddressInfo &info : *resolved)
    {
        if (open(info, *address, timeout))
        {
            return true;
        }
    }

    return false;
}

bool WebsocketClient::open(ws_client::AddressInfo &address, const ws_client::URL &url, milliseconds timeout) noexcept
{
    ws_client::TcpSocket<Logger> tcp(&m_logger, std::move(address));

    const auto created = tcp.init();
    if (!created.has_value())
    {
        fail(created.error());
        return false;
    }

    const int fd = tcp.fd();

    Socket socket(&m_logger, std::move(tcp), &m_context, url.host(), true);

    const auto prepared = socket.init();
    if (!prepared.has_value())
    {
        fail(prepared.error());
        return false;
    }

    const auto established = socket.connect(timeout);
    if (!established.has_value())
    {
        fail(established.error());
        return false;
    }

    m_client.emplace(&m_logger, std::move(socket));

    ws_client::Handshake<Logger> handshake(&m_logger, url);

    const auto upgraded = m_client->handshake(handshake, timeout);
    if (!upgraded.has_value())
    {
        fail(upgraded.error());
        m_client.reset();

        return false;
    }

    m_fd = fd;
    m_close_code = ws_client::close_code::normal_closure;
    m_error[0] = '\0';

    return true;
}

void WebsocketClient::close() noexcept
{
    if (!m_client.has_value())
    {
        return;
    }

    [[maybe_unused]] const auto result = m_client->close(m_close_code, kCloseTimeout);

    m_client.reset();
    m_fd = -1;
    m_close_code = ws_client::close_code::not_set;
}

bool WebsocketClient::send(std::string_view text) noexcept
{
    if (!m_client.has_value()) [[unlikely]]
    {
        fail("websocket is not connected");
        return false;
    }

    if (text.size() > kWriteCapacity) [[unlikely]]
    {
        fail("websocket message is too long");
        return false;
    }

    std::copy(text.begin(), text.end(), m_output);

    const ws_client::Message message(ws_client::MessageType::text, std::string_view(m_output, text.size()));

    const auto result = m_client->send_message(message, { .compress = false, .timeout = kWriteTimeout });
    if (!result.has_value()) [[unlikely]]
    {
        fail(result.error());
        return false;
    }

    return true;
}

WebsocketStatus WebsocketClient::read(std::string_view &text, milliseconds timeout) noexcept
{
    text = std::string_view();

    if (!m_client.has_value()) [[unlikely]]
    {
        fail("websocket is not connected");
        return WebsocketStatus::Failed;
    }

    auto result = m_client->read_message(*m_buffer, timeout);

    if (const ws_client::Message *message = std::get_if<ws_client::Message>(&result)) [[likely]]
    {
        text = message->to_string_view();
        return WebsocketStatus::Message;
    }

    if (ws_client::PingFrame *ping = std::get_if<ws_client::PingFrame>(&result))
    {
        const auto pong = m_client->send_pong_frame(ping->payload_bytes(), kWriteTimeout);
        if (!pong.has_value()) [[unlikely]]
        {
            fail(pong.error());
            return WebsocketStatus::Failed;
        }

        return WebsocketStatus::Control;
    }

    if (std::holds_alternative<ws_client::PongFrame>(result))
    {
        return WebsocketStatus::Control;
    }

    if (const ws_client::CloseFrame *frame = std::get_if<ws_client::CloseFrame>(&result))
    {
        const auto written = std::format_to_n(m_error, kErrorCapacity - 1, "close code = {}, reason = {}",
                                              static_cast<int>(frame->get_close_code()), frame->get_reason());
        *written.out = '\0';

        m_close_code = ws_client::close_code::normal_closure;
        return WebsocketStatus::Closed;
    }

    fail(std::get<ws_client::WSError>(result));
    return WebsocketStatus::Failed;
}

bool WebsocketClient::pending() noexcept
{
    if (!m_client.has_value())
    {
        return false;
    }

    ws_client::Timeout<> expired(milliseconds(1), {});

    const auto result = m_client->underlying().wait_readable(expired);
    return result.has_value() && *result;
}

bool WebsocketClient::connected() const noexcept
{
    return m_client.has_value();
}

int WebsocketClient::fd() const noexcept
{
    return m_fd;
}

std::string_view WebsocketClient::error() const noexcept
{
    return std::string_view(m_error);
}

void WebsocketClient::fail(const ws_client::WSError &error) noexcept
{
    const auto written = std::format_to_n(m_error, kErrorCapacity - 1, "{}: {}", ws_client::to_string(error.code), error.message);
    *written.out = '\0';

    m_close_code = error.close_with_code;
}

void WebsocketClient::fail(std::string_view message) noexcept
{
    const auto written = std::format_to_n(m_error, kErrorCapacity - 1, "{}", message);
    *written.out = '\0';

    m_close_code = ws_client::close_code::not_set;
}

} // namespace nlx::network
