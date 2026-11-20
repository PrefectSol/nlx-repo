#ifndef NLX_SERVICE_ACCOUNT_H
#define NLX_SERVICE_ACCOUNT_H

#include "core/credentials.h"
#include "core/file_descriptor.h"
#include "core/market.h"
#include "core/metrics.h"
#include "core/spsc_queue.h"
#include "core/time.h"
#include "exchange/okx_rate_limiter.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"
#include "service/executor.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nlx::service
{

template<typename Streamer>
class Account
{
public:
    explicit Account(const Credentials &credentials, exchange::OkxRateLimiter &limiter, const char *url, logging::LoggerRegistry &registry);

    Account(const Account &) = delete;
    Account &operator=(const Account &) = delete;
    Account(Account &&) = delete;
    Account &operator=(Account &&) = delete;

    void initialize();
    void start() noexcept;
    void request_stop() noexcept;

    [[nodiscard]] bool submit(const ExecutorCommand &command) noexcept;

    [[nodiscard]] const FileDescriptor &events() const noexcept;
    [[nodiscard]] bool pop(AccountEvent &event) noexcept;

private:
    static constexpr size_t kTimerMilliseconds = 500;
    static constexpr size_t kDrainMilliseconds = 20000;
    static constexpr size_t kCommandsCapacity = 64;
    static constexpr size_t kEventsCapacity = 256;
    static constexpr size_t kMetricsTicks = 120;

    logging::LoggerNode m_log;
    Streamer m_streamer;
    Executor<Streamer> m_executor;

    FileDescriptor m_epoll;
    FileDescriptor m_stop;
    FileDescriptor m_command;
    FileDescriptor m_timer;
    FileDescriptor m_event;

    std::atomic<bool> m_initialized;

    SpscQueue<ExecutorCommand, kCommandsCapacity> m_commands;
    SpscQueue<AccountEvent, kEventsCapacity> m_events;

    Balance m_balance;
    bool m_connected;

    LoopMeter m_loop;
    LatencyHistogram m_receive;
    LatencyHistogram m_command_delay;
    size_t m_ticks;

    void receive() noexcept;
    void command() noexcept;
    void tick() noexcept;
    void drain() noexcept;

    void balance() noexcept;
    void positions() noexcept;
    void orders() noexcept;

    void forward() noexcept;
    void emit(const AccountEvent &event) noexcept;
};

} // namespace nlx::service
#endif // !NLX_SERVICE_ACCOUNT_H
