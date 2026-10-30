#include "supervisor.h"
#include "core/time.h"
#include "core/credentials.h"
#include "core/file_descriptor.h"
#include "core/system_error.h"
#include "core/fatal.h"
#include "core/thread.h"
#include "logging/logger_registry.h"

#include <sched.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <csignal>
#include <stdexcept>

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

Supervisor::Supervisor(const Config &config, const Credentials &credentials) :
    m_config(config),
    m_credentials(credentials),
    m_logger(m_config.logger),
    m_registry(m_logger),
    m_log(),
    m_okx_rate_limiter(),
    m_okx_http_client(m_config.okx, m_credentials, m_okx_rate_limiter, m_registry),
    m_okx_websocket(m_config.okx, m_credentials, m_okx_rate_limiter, m_registry),
    m_strategy(m_config.strategy, m_okx_http_client, m_okx_websocket, m_registry),
    m_logger_thread(),
    m_okx_http_thread(),
    m_okx_websocket_thread(),
    m_strategy_thread(),
    m_epoll(),
    m_signals(),
    m_stop()
{
    if (!time::synchronized())
    {
        throw std::runtime_error("system clock is not synchronized");
    }

    m_log = m_registry.create_node("supervisor");

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

    if (m_okx_websocket_thread.joinable())
    {
        m_okx_websocket.request_stop();
        m_okx_websocket_thread.join();
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
    constexpr size_t logger_stack_size = 2 * 1024 * 1024;
    constexpr size_t okx_http_stack_size = 4 * 1024 * 1024;
    constexpr size_t okx_websocket_stack_size = 4 * 1024 * 1024;
    constexpr size_t strategy_stack_size = 8 * 1024 * 1024;

    const pthread_t handle = thread::this_native_handle();

    bool configured = thread::set_name(handle, "nlx-supervisor");
    configured = configured && thread::set_affinity(handle, {});
    configured = configured && thread::set_scheduling(handle, SCHED_OTHER, 0);

    if (!configured) [[unlikely]]
    {
        NLX_FATAL("nlx::Supervisor::start() main thread setup failed");
    }

    m_logger.initialize();
    m_logger_thread.start(m_logger, "nlx-logger", logger_stack_size, SCHED_OTHER, 0, {});

    m_log.log("supervisor started");
    m_log.log("logger directory = {}", m_config.logger.directory);
    m_log.log("logger rotation minutes = {}", m_config.logger.rotation_minutes);
    m_log.log("logger flush ms = {}", m_config.logger.flush_ms);
    m_log.log("okx api url = {}", m_config.okx.url);
    m_log.log("okx websocket url = {}", m_config.okx.websocket_url);
    m_log.log("okx demo trading mode = {}", m_config.okx.simulated);
    m_log.log("strategy instruments refresh minutes = {}", m_config.strategy.instruments_refresh_minutes);
    m_log.flush();

    try
    {
        m_okx_http_client.initialize();
        m_okx_http_thread.start(m_okx_http_client, "nlx-okx_http", okx_http_stack_size, SCHED_OTHER, 0, {});

        m_okx_websocket.initialize();
        m_okx_websocket_thread.start(m_okx_websocket, "nlx-okx_ws", okx_websocket_stack_size, SCHED_OTHER, 0, {});

        m_strategy.initialize();
        m_strategy_thread.start(m_strategy, "nlx-strategy", strategy_stack_size, SCHED_OTHER, 0, {});
    }
    catch (const std::exception &exp)
    {
        m_log.log<logging::LogLevel::Error>("supervisor start failed: {}", exp.what());
        m_log.flush();

        throw;
    }

    epoll_event event{};
    while (true)
    {
        const size_t count = detail::epoll_wait(m_epoll, { &event, 1 }, -1);
        if (count == 0)
        {
            continue;
        }

        if (event.data.fd == m_stop)
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
