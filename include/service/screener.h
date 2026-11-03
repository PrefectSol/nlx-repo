#ifndef NLX_SERVICE_SCREENER_H
#define NLX_SERVICE_SCREENER_H

#include "core/file_descriptor.h"
#include "core/time.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"

#include <atomic>
#include <cstddef>
#include <span>
#include <string_view>

namespace nlx::service
{

struct ScreenerRequest
{
    std::span<const std::string_view> instruments;
    size_t accepted;
    const FileDescriptor &notify;
};

template<typename Streamer>
class Screener
{
public:
    explicit Screener(Streamer &streamer, logging::LoggerRegistry &registry, std::string_view name);

    Screener(const Screener &) = delete;
    Screener &operator=(const Screener &) = delete;
    Screener(Screener &&) = delete;
    Screener &operator=(Screener &&) = delete;

    void initialize();
    void start() noexcept;
    void request_stop() noexcept;

    [[nodiscard]] bool resubscribe(ScreenerRequest &request) noexcept;

private:
    static constexpr milliseconds kTickInterval{ 1000 };

    logging::LoggerNode m_log;
    Streamer &m_streamer;

    FileDescriptor m_epoll;
    FileDescriptor m_stop;
    FileDescriptor m_update;
    FileDescriptor m_timer;

    std::atomic<bool> m_initialized;
    std::atomic<ScreenerRequest *> m_request;

    void update() noexcept;
};

} // namespace nlx::service
#endif // !NLX_SERVICE_SCREENER_H
