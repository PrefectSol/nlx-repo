#ifndef NLX_SERVICE_STRATEGY_H
#define NLX_SERVICE_STRATEGY_H

#include "core/file_descriptor.h"
#include "core/time.h"
#include "exchange/okx_http_client.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"
#include "network/http_client.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nlx::service
{

class Strategy
{
public:
    explicit Strategy(exchange::OkxHttpClient &okx_http_client, logging::LoggerRegistry &registry);

    Strategy(const Strategy &) = delete;
    Strategy &operator=(const Strategy &) = delete;
    Strategy(Strategy &&) = delete;
    Strategy &operator=(Strategy &&) = delete;

    void initialize();
    void start() noexcept;
    void request_stop() noexcept;

private:
    static constexpr size_t kBufferCapacity = 256 * 1024;
    static constexpr uint32_t kOrdersLimit = 100;

    logging::LoggerNode m_log;
    exchange::OkxHttpClient &m_okx_http_client;

    FileDescriptor m_epoll;
    FileDescriptor m_stop;
    FileDescriptor m_okx_notify;

    std::atomic<bool> m_initialized;

    bool okx_get_system_time(network::HttpRequest &request, int &code, milliseconds &timestamp) noexcept;
    bool okx_get_account_config(network::HttpRequest &request, int &code, uint64_t &uid, int &mode, bool &hedge, bool &trade) noexcept;
    bool okx_get_positions(network::HttpRequest &request, int &code, size_t &count) noexcept;
    bool okx_get_orders(network::HttpRequest &request, int &code, size_t &count) noexcept;
    bool okx_set_hedge_mode(network::HttpRequest &request, int &code) noexcept;
    bool okx_get_balance(network::HttpRequest &request, int &code, double &available, std::string_view currency) noexcept;
};

} // namespace nlx::service
#endif // !NLX_SERVICE_STRATEGY_H
