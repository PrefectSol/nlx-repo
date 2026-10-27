#ifndef NLX_SUPERVISOR_H
#define NLX_SUPERVISOR_H

#include "core/config.h"
#include "core/credentials.h"
#include "core/file_descriptor.h"
#include "core/thread.h"
#include "exchange/okx_http_client.h"
#include "exchange/okx_rate_limiter.h"
#include "logging/logger.h"
#include "logging/logger_registry.h"
#include "service/strategy.h"

#include <cstddef>

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
    static constexpr size_t kLoggerStackSize = 2 * 1024 * 1024;
    static constexpr size_t kOkxHttpStackSize = 2 * 1024 * 1024;
    static constexpr size_t kStrategyStackSize = 8 * 1024 * 1024;

    Config m_config;
    Credentials m_credentials;

    logging::Logger m_logger;
    logging::LoggerRegistry m_registry;
    logging::LoggerNode m_log;

    exchange::OkxRateLimiter m_okx_rate_limiter;
    exchange::OkxHttpClient m_okx_http_client;

    service::Strategy m_strategy;

    Thread m_logger_thread;
    Thread m_okx_http_thread;
    Thread m_strategy_thread;

    FileDescriptor m_epoll;
    FileDescriptor m_signals;
    FileDescriptor m_stop;
};

} // namespace nlx
#endif // !NLX_SUPERVISOR_H
