#include "supervisor.h"
#include "logging/logger_registry.h"

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <csignal>
#include <utility>

namespace nlx
{

namespace 
{

constexpr std::string_view signo_name(int signo) noexcept
{
    switch (signo)
    {
        case SIGINT:
            return "SIGINT";
        case SIGTERM:
            return "SIGTERM";
    }

    return "UNKNOWN";
}

} // namespace

Supervisor::Supervisor(Config config) :
    m_config(std::move(config)),
    m_running(false),
    m_logger(m_config.logger),
    m_log(),
    m_logger_thread(),
    m_epoll(),
    m_signals()
{
    logging::LoggerRegistry registry(m_logger);
    m_log = registry.create_node("supervisor");
    
    m_epoll = FileDescriptor::make_epoll();
    m_signals = FileDescriptor::make_signalfd({ SIGINT, SIGTERM }, true);
    
    nlx::detail::epoll_add(m_epoll, m_signals);
}

Supervisor::~Supervisor()
{
    request_stop();
}

int Supervisor::start()
{
    m_running.store(true, std::memory_order_release);
    
    m_logger_thread = std::thread([this] { m_logger.run(); });

    m_log.log("supervisor started");
    m_log.log("logger directory = {}", m_config.logger.directory);
    m_log.log("logger rotation minutes = {}", m_config.logger.rotation_minutes);
    m_log.log("logger flush ms = {}", m_config.logger.flush_ms);
    m_log.log("logger console = {}", m_config.logger.console);
    m_log.flush();

    epoll_event event{};
    while (true)
    {
        const size_t count = nlx::detail::epoll_wait(m_epoll, { &event, 1 }, -1);
        if (count == 0)
        {
            continue;
        }

        const int signo = nlx::detail::receive(m_signals);
        if (signo == SIGINT || signo == SIGTERM)
        {
            m_log.log<logging::LogLevel::Warning>("received signal {}", signo_name(signo));
            break;
        }
    }

    m_log.log("supervisor stopped");
    m_log.flush();

    return 0;
}

void Supervisor::request_stop() noexcept
{
    m_running.store(false, std::memory_order_release);
    
    if (m_logger_thread.joinable())
    {
        m_logger.wake();
        m_logger_thread.join();
    }
}

} // namespace nlx
