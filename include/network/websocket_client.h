#ifndef NLX_NETWORK_WEBSOCKET_CLIENT_H
#define NLX_NETWORK_WEBSOCKET_CLIENT_H

#include "core/time.h"

#include <ws_client/ws_client.hpp>
#include <ws_client/transport/builtin/DnsResolver.hpp>
#include <ws_client/transport/builtin/OpenSslContext.hpp>
#include <ws_client/transport/builtin/OpenSslSocket.hpp>
#include <ws_client/transport/builtin/TcpSocket.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
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

class WebsocketClient
{
public:
    WebsocketClient() noexcept;
    ~WebsocketClient() noexcept;

    WebsocketClient(const WebsocketClient &) = delete;
    WebsocketClient &operator=(const WebsocketClient &) = delete;
    WebsocketClient(WebsocketClient &&) = delete;
    WebsocketClient &operator=(WebsocketClient &&) = delete;

    void initialize();

    [[nodiscard]] bool connect(std::string_view url, milliseconds timeout) noexcept;
    void close() noexcept;

    [[nodiscard]] std::span<char> output() noexcept;
    [[nodiscard]] bool send(size_t size) noexcept;

    [[nodiscard]] WebsocketStatus read(std::string_view &text, milliseconds timeout) noexcept;
    [[nodiscard]] bool pending(milliseconds timeout) noexcept;

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] std::string_view error() const noexcept;

private:
    using Logger = ws_client::ConsoleLogger;
    using Context = ws_client::OpenSslContext<Logger>;
    using Socket = ws_client::OpenSslSocket<Logger>;
    using Client = ws_client::WebSocketClient<Logger, Socket>;

    static constexpr size_t kReadCapacity = 1024 * 1024;
    static constexpr size_t kWriteCapacity = 64 * 1024;
    static constexpr size_t kErrorCapacity = 256;
    static constexpr size_t kWriteMilliseconds = 5000;
    static constexpr size_t kCloseMilliseconds = 1000;

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
#endif // !NLX_NETWORK_WEBSOCKET_CLIENT_H
