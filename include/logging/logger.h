#ifndef NLX_LOGGING_LOGGER_H
#define NLX_LOGGING_LOGGER_H

#include "core/file_descriptor.h"
#include "core/metrics.h"
#include "core/time.h"
#include "logging/logger_node.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <string_view>

namespace nlx::logging
{

class LoggerRegistry;

class Logger
{
public:
    explicit Logger();

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
    Logger(Logger &&) = delete;
    Logger &operator=(Logger &&) = delete;

    void initialize();
    void start() noexcept;
    void request_stop() noexcept;

    [[nodiscard]] std::string_view get_directory() const noexcept;
    [[nodiscard]] size_t get_rotation_minutes() const noexcept;
    [[nodiscard]] size_t get_flush_milliseconds() const noexcept;

private:
    friend class LoggerRegistry;

    static constexpr char kDirectory[] = "logs/";
    static constexpr size_t kRotationMinutes = 1440;
    static constexpr size_t kFlushMilliseconds = 100;
    static constexpr size_t kMetricsMilliseconds = 60000;

    static constexpr size_t kModulesSize = 8;
    static constexpr size_t kNameSize = 16;

    static constexpr char kRotationFormat[] = "{:%Y%m%d-%H%M}";
    static constexpr char kDateFormat[] = "{:%F %T}";
    static constexpr char kTruncatedMarker[] = "[truncated]";
    static constexpr char kFileExtension[] = ".log";
    
    static constexpr size_t kDateSize = 32;
    static constexpr size_t kLineSize = 4096;
    
    struct Module
    {
        LoggerChannel channel;
        char name[kNameSize]{};

        FileDescriptor file;
        nanoseconds rotation{};

        size_t records{};
        size_t dropped{};
        size_t peak{};
    };

    std::atomic<bool> m_initialized;

    FileDescriptor m_epoll;
    FileDescriptor m_notify;
    FileDescriptor m_stop;

    std::array<Module, kModulesSize> m_modules;
    size_t m_registered;

    Module m_self;
    LoopMeter m_loop;
    LatencyHistogram m_write;
    LatencyHistogram m_lag;
    size_t m_bytes;
    nanoseconds m_reported;

    LoggerNode create_node(std::string_view name);

    void drain_modules() noexcept; 
    size_t drain_module(Module &module) noexcept; 

    void write_record(Module &module, const LogRecord &record) noexcept;
    void write_dropped(const Module &module, size_t dropped) noexcept;
    void write_text(Module &module, LogLevel level, std::string_view text) noexcept;
    void write(Module &module, std::string_view text) noexcept;

    void report(nanoseconds now) noexcept;

    void rotate(Module &module, nanoseconds timestamp) noexcept;
};

} // namespace nlx::logging
#endif // !NLX_LOGGING_LOGGER_H
