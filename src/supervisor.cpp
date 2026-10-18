#include "supervisor.h"
#include "core/credentials.h"
#include "core/file_descriptor.h"
#include "logging/logger_registry.h"
#include "core/system_error.h"
#include "core/time.h"

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <thread>
#include <unistd.h>

#include <csignal>
#include <stdexcept>
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

Supervisor::Supervisor(Config config, Credentials credentials) :
    m_config(std::move(config)),
    m_credentials(std::move(credentials)),
    m_started(false),
    m_logger(m_config.logger),
    m_registry(m_logger),
    m_log(m_registry.create_node("supervisor")),
    m_okx_rate_limiter(),
    m_okx_http_client(m_config.okx, m_credentials, m_okx_rate_limiter, m_registry),
    m_strategy(m_okx_http_client, m_registry),
    m_logger_thread(),
    m_okx_http_thread(),
    m_strategy_thread(),
    m_epoll(),
    m_signals(),
    m_stop()
{
    if (!time::synchronized())
    {
        throw std::runtime_error("system clock is not synchronized");
    }

    m_epoll = FileDescriptor::make_epoll();
    m_signals = FileDescriptor::make_signalfd({ SIGINT, SIGTERM }, true);
    m_stop = FileDescriptor::make_eventfd(true);

    const sighandler_t result = ::signal(SIGPIPE, SIG_IGN);
    if (result == SIG_ERR) [[unlikely]]
    {
        throw SystemError("signal failed");
    }

    detail::epoll_add(m_epoll, m_signals);
    detail::epoll_add(m_epoll, m_stop);
}

Supervisor::~Supervisor()
{
    if (m_strategy_thread.joinable())
    {
        m_strategy.request_stop();
        m_strategy_thread.join();
    }

    if (m_okx_http_thread.joinable())
    {
        m_okx_http_client.request_stop();
        m_okx_http_thread.join();
    }

    if (m_logger_thread.joinable())
    {
        m_logger.request_stop();
        m_logger_thread.join();
    }
}

int Supervisor::start()
{
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return -1;
    }

    m_logger_thread = std::thread([this] { m_logger.start(); });
    
    m_log.log("supervisor started");
    m_log.log("logger directory = {}", m_config.logger.directory);
    m_log.log("logger rotation minutes = {}", m_config.logger.rotation_minutes);
    m_log.log("logger flush ms = {}", m_config.logger.flush_ms);
    m_log.log("okx demo trading mode = {}", m_config.okx.simulated);
    m_log.flush();
    
    m_okx_http_thread = std::thread([this] { m_okx_http_client.start(); });

    if (m_strategy.initialize(m_epoll))
    {
        m_strategy_thread = std::thread([this] { m_strategy.start(); });
    }

    epoll_event event{};
    while (true)
    {
        const size_t count = detail::epoll_wait(m_epoll, { &event, 1 }, -1);
        if (count == 0)
        {
            continue;
        }

        if (event.data.fd == m_stop.get())
        {
            [[maybe_unused]] const uint64_t result = detail::drain(m_stop);
            m_log.log<logging::LogLevel::Warning>("stop requested");
            break;
        }

        const int signo = detail::receive(m_signals);
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
    detail::notify(m_stop); 
}

} // namespace nlx
