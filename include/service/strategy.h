#ifndef NLX_SERVICE_STRATEGY_H
#define NLX_SERVICE_STRATEGY_H

#include "core/config.h"
#include "core/credentials.h"
#include "core/file_descriptor.h"
#include "exchange/okx_http_client.h"
#include "exchange/okx_rate_limiter.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"
#include "network/http_client.h"
#include "parsing/json_reader.h"

#include <cstddef>
#include <thread>

namespace nlx::service
{

class Strategy
{
public:
    explicit Strategy(const OkxConfig &config, const Credentials &credentials, logging::LoggerRegistry &registry);
    ~Strategy();

    Strategy(const Strategy &) = delete;
    Strategy &operator=(const Strategy &) = delete;
    Strategy(Strategy &&) = delete;
    Strategy &operator=(Strategy &&) = delete;

    void initialize();
    void start() noexcept;
    void request_stop() noexcept;
    void shutdown() noexcept;

private:
    static constexpr size_t kBufferCapacity = 16384;

    static_assert(kBufferCapacity > parsing::json_padding_size);

    logging::LoggerNode m_log;

    exchange::OkxRateLimiter m_okx_rate_limiter;
    exchange::OkxHttpClient m_okx_http_client;

    std::thread m_okx_http_thread;

    FileDescriptor m_epoll;
    FileDescriptor m_stop;

    double m_available_balance;

    void authorize(network::HttpRequest &request);
    void check_positions(network::HttpRequest &request);
    void load_balance(network::HttpRequest &request);
    void synchronize(network::HttpRequest &request);
};

} // namespace nlx::service
#endif // !NLX_SERVICE_STRATEGY_H
