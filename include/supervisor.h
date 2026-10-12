#ifndef NLX_SUPERVISOR_H
#define NLX_SUPERVISOR_H

#include "core/config.h"
#include "core/file_descriptor.h"
#include "logging/logger.h"

#include <thread>
#include <atomic>

namespace nlx
{

class Supervisor
{
public:
    explicit Supervisor(Config config);
    ~Supervisor();

    Supervisor(const Supervisor &) = delete;
    Supervisor &operator=(const Supervisor &) = delete;
    Supervisor(Supervisor &&) = delete;
    Supervisor &operator=(Supervisor &&) = delete;

    [[nodiscard]] int start();
    void request_stop();

private:
    Config m_config;

    std::atomic<bool> m_running;

    logging::Logger m_logger;
    logging::LoggerNode m_log;

    std::thread m_logger_thread;

    FileDescriptor m_epoll;
    FileDescriptor m_signals;
};

} // namespace nlx
#endif // !NLX_SUPERVISOR_H
