#ifndef NLX_SUPERVISOR_H
#define NLX_SUPERVISOR_H

#include "core/config.h"
#include "core/credentials.h"
#include "core/file_descriptor.h"
#include "logging/logger.h"

#include <thread>
#include <atomic>

namespace nlx
{

class Supervisor
{
public:
    explicit Supervisor(Config config, Credentials credentials);
    ~Supervisor();

    Supervisor(const Supervisor &) = delete;
    Supervisor &operator=(const Supervisor &) = delete;
    Supervisor(Supervisor &&) = delete;
    Supervisor &operator=(Supervisor &&) = delete;

    [[nodiscard]] int run();
    void request_stop() noexcept;

private:
    Config m_config;
    Credentials m_credentials;

    std::atomic<bool> m_started;

    logging::Logger m_logger;
    logging::LoggerNode m_log;

    std::thread m_logger_thread;

    FileDescriptor m_epoll;
    FileDescriptor m_signals;
    FileDescriptor m_stop;
};

} // namespace nlx
#endif // !NLX_SUPERVISOR_H
