#ifndef NLX_SERVICE_ACCOUNT_H
#define NLX_SERVICE_ACCOUNT_H

#include "core/credentials.h"
#include "core/file_descriptor.h"
#include "core/market.h"
#include "core/spsc_queue.h"
#include "exchange/okx_rate_limiter.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nlx::service
{

struct AccountEvent
{
    Balance balance;
    int64_t timestamp;
    bool connected;
};

template<typename Streamer>
class Account
{
public:
    explicit Account(const Credentials &credentials, exchange::OkxRateLimiter &limiter, logging::LoggerRegistry &registry);

    Account(const Account &) = delete;
    Account &operator=(const Account &) = delete;
    Account(Account &&) = delete;
    Account &operator=(Account &&) = delete;

    void initialize();
    void start() noexcept;
    void request_stop() noexcept;

    [[nodiscard]] const FileDescriptor &events() const noexcept;
    [[nodiscard]] bool pop(AccountEvent &event) noexcept;

private:
    static constexpr size_t kTimerMilliseconds = 1000;
    static constexpr size_t kEventsCapacity = 64;

    logging::LoggerNode m_log;
    Streamer m_streamer;

    FileDescriptor m_epoll;
    FileDescriptor m_stop;
    FileDescriptor m_timer;
    FileDescriptor m_event;

    std::atomic<bool> m_initialized;

    SpscQueue<AccountEvent, kEventsCapacity> m_events;

    Balance m_balance;
    bool m_connected;

    void receive() noexcept;

    void balance() noexcept;
    void positions() noexcept;
    void orders() noexcept;

    void emit(const AccountEvent &event) noexcept;
};

} // namespace nlx::service
#endif // !NLX_SERVICE_ACCOUNT_H
