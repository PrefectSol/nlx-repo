#ifndef NLX_SUPERVISOR_H
#define NLX_SUPERVISOR_H

#include "core/config.h"
#include "core/credentials.h"
#include "core/file_descriptor.h"
#include "core/thread.h"
#include "exchange/okx_http_client.h"
#include "exchange/okx_rate_limiter.h"
#include "exchange/okx_websocket.h"
#include "logging/logger.h"
#include "logging/logger_registry.h"
#include "service/strategy.h"

namespace nlx
{

class Supervisor
{
public:
    explicit Supervisor(const Config &config, const Credentials &credentials);
    ~Supervisor();

    Supervisor(const Supervisor &) = delete;
    Supervisor &operator=(const Supervisor &) = delete;
    Supervisor(Supervisor &&) = delete;
    Supervisor &operator=(Supervisor &&) = delete;

    [[nodiscard]] int start();
    void request_stop() noexcept;

private:
    Config m_config;
    Credentials m_credentials;

    logging::Logger m_logger;
    logging::LoggerRegistry m_registry;
    logging::LoggerNode m_log;

    exchange::OkxRateLimiter m_okx_rate_limiter;
    exchange::OkxHttpClient m_okx_http_client;
    exchange::OkxWebsocket m_okx_websocket;

    service::Strategy m_strategy;

    thread::Thread m_logger_thread;
    thread::Thread m_okx_http_thread;
    thread::Thread m_okx_websocket_thread;
    thread::Thread m_strategy_thread;

    FileDescriptor m_epoll;
    FileDescriptor m_signals;
    FileDescriptor m_stop;
};

} // namespace nlx
#endif // !NLX_SUPERVISOR_H
