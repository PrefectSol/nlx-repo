#ifndef NLX_SERVICE_STRATEGY_H
#define NLX_SERVICE_STRATEGY_H

#include "core/file_descriptor.h"
#include "core/time.h"
#include "exchange/okx_http_client.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"
#include "network/http_client.h"
#include "parsing/json_reader.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

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

    [[nodiscard]] bool initialize(const FileDescriptor &interrupt);
    void start() noexcept;
    void request_stop() noexcept;

private:
    static constexpr size_t kBufferCapacity = 16384;

    static_assert(kBufferCapacity > parsing::json_padding_size);

    logging::LoggerNode m_log;
    exchange::OkxHttpClient &m_okx_http_client;

    FileDescriptor m_epoll;
    FileDescriptor m_stop;
    FileDescriptor m_okx_notify;

    std::atomic<bool> m_initialized;

    void prepare(network::HttpRequest &request, const FileDescriptor &interrupt);

    bool okx_get_account_config(network::HttpRequest &request, const FileDescriptor &interrupt,
                                int &code, uint64_t &uid, int &mode, bool &hedge, bool &trade) noexcept;
    bool okx_get_positions(network::HttpRequest &request, const FileDescriptor &interrupt, int &code, size_t &count) noexcept;
    bool okx_get_orders(network::HttpRequest &request, const FileDescriptor &interrupt, int &code, bool &open) noexcept;
    bool okx_set_hedge_mode(network::HttpRequest &request, const FileDescriptor &interrupt, int &code) noexcept;
    bool okx_get_balance(network::HttpRequest &request, const FileDescriptor &interrupt, int &code, double &available) noexcept;
    bool okx_get_system_time(network::HttpRequest &request, const FileDescriptor &interrupt, int &code, milliseconds &timestamp) noexcept;
    bool okx_await(bool queued, network::HttpRequest &request, const FileDescriptor &interrupt) noexcept;
};

} // namespace nlx::service
#endif // !NLX_SERVICE_STRATEGY_H
