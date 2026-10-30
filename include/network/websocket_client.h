#ifndef NLX_NETWORK_WEBSOCKET_CLIENT_H
#define NLX_NETWORK_WEBSOCKET_CLIENT_H

#include "core/time.h"
#include "logging/logger_node.h"

#include <ws_client/ws_client.hpp>
#include <ws_client/transport/builtin/DnsResolver.hpp>
#include <ws_client/transport/builtin/OpenSslContext.hpp>
#include <ws_client/transport/builtin/OpenSslSocket.hpp>
#include <ws_client/transport/builtin/TcpSocket.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <source_location>
#include <string_view>

namespace nlx::network
{

enum class WebsocketStatus : uint8_t
{
    Message,
    Control,
    Closed,
    Failed
};

namespace detail
{

class WebsocketLogger
{
public:
    WebsocketLogger() noexcept;

    WebsocketLogger(const WebsocketLogger &) = delete;
    WebsocketLogger &operator=(const WebsocketLogger &) = delete;
    WebsocketLogger(WebsocketLogger &&) = delete;
    WebsocketLogger &operator=(WebsocketLogger &&) = delete;

    void attach(logging::LoggerNode &log) noexcept;

    template<ws_client::LogLevel Level, ws_client::LogTopic Topic>
    [[nodiscard]] bool is_enabled() const noexcept;

    template<ws_client::LogLevel Level, ws_client::LogTopic Topic>
    void log(std::string_view message, std::source_location location = std::source_location::current()) noexcept;

private:
    logging::LoggerNode *m_log;
};

} // namespace nlx::network::detail

class WebsocketClient
{
public:
    WebsocketClient() noexcept;
    ~WebsocketClient() noexcept;

    WebsocketClient(const WebsocketClient &) = delete;
    WebsocketClient &operator=(const WebsocketClient &) = delete;
    WebsocketClient(WebsocketClient &&) = delete;
    WebsocketClient &operator=(WebsocketClient &&) = delete;

    void initialize(logging::LoggerNode &log);

    [[nodiscard]] bool connect(std::string_view url, milliseconds timeout) noexcept;
    void close() noexcept;

    [[nodiscard]] bool send(std::string_view text) noexcept;
    [[nodiscard]] WebsocketStatus read(std::string_view &text, milliseconds timeout) noexcept;
    [[nodiscard]] bool pending() noexcept;

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] std::string_view error() const noexcept;

    static constexpr size_t kReadCapacity = 1024 * 1024;
    static constexpr size_t kWriteCapacity = 64 * 1024;

private:
    using Logger = detail::WebsocketLogger;
    using Context = ws_client::OpenSslContext<Logger>;
    using Socket = ws_client::OpenSslSocket<Logger>;
    using Client = ws_client::WebSocketClient<Logger, Socket>;

    static constexpr size_t kErrorCapacity = 256;
    static constexpr milliseconds kWriteTimeout{ 5000 };
    static constexpr milliseconds kCloseTimeout{ 1000 };

    Logger m_logger;
    Context m_context;

    std::optional<ws_client::Buffer> m_buffer;
    std::optional<Client> m_client;

    int m_fd;
    ws_client::close_code m_close_code;

    alignas(64) char m_output[kWriteCapacity];
    char m_error[kErrorCapacity];

    bool open(ws_client::AddressInfo &address, const ws_client::URL &url, milliseconds timeout) noexcept;

    void fail(const ws_client::WSError &error) noexcept;
    void fail(std::string_view message) noexcept;
};

} // namespace nlx::network

namespace nlx::network::detail
{

template<ws_client::LogLevel Level, ws_client::LogTopic Topic>
bool WebsocketLogger::is_enabled() const noexcept
{
    return m_log != nullptr && Level == ws_client::LogLevel::E;
}

template<ws_client::LogLevel Level, ws_client::LogTopic Topic>
void WebsocketLogger::log(std::string_view message, std::source_location location) noexcept
{
    if (!is_enabled<Level, Topic>())
    {
        return;
    }

    const std::string_view topic = ws_client::to_string(Topic);
    const std::string_view file = ws_client::extract_log_file_name(location.file_name());

    m_log->log<logging::LogLevel::Error>("websocket {} {}:{} {}", topic, file, location.line(), message);
}

} // namespace nlx::network::detail
#endif // !NLX_NETWORK_WEBSOCKET_CLIENT_H
