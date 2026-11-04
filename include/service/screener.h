#ifndef NLX_SERVICE_SCREENER_H
#define NLX_SERVICE_SCREENER_H

#include "core/file_descriptor.h"
#include "exchange/okx_rate_limiter.h"
#include "exchange/okx_types.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"

#include <atomic>
#include <cstddef>
#include <span>

namespace nlx::service
{

template<typename Streamer>
class Screener
{
public:
    explicit Screener(exchange::OkxRateLimiter &limiter, logging::LoggerRegistry &registry);

    Screener(const Screener &) = delete;
    Screener &operator=(const Screener &) = delete;
    Screener(Screener &&) = delete;
    Screener &operator=(Screener &&) = delete;

    void initialize();
    void start() noexcept;
    void request_stop() noexcept;

    [[nodiscard]] bool resubscribe(std::span<const exchange::OkxInstrument> instruments) noexcept;

private:
    static constexpr size_t kTimerMilliseconds = 1000;

    struct Request
    {
        std::span<const exchange::OkxInstrument> instruments;
        bool result;
    };

    logging::LoggerNode m_log;
    Streamer m_streamer;

    FileDescriptor m_epoll;
    FileDescriptor m_stop;
    FileDescriptor m_update;
    FileDescriptor m_done;
    FileDescriptor m_timer;

    std::atomic<bool> m_initialized;
    std::atomic<Request *> m_request;

    void update() noexcept;
};

} // namespace nlx::service
#endif // !NLX_SERVICE_SCREENER_H
