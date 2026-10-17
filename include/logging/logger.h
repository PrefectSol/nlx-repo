#ifndef NLX_LOGGING_LOGGER_H
#define NLX_LOGGING_LOGGER_H

#include "core/config.h"
#include "core/file_descriptor.h"
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
    explicit Logger(LoggerConfig config);

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
    Logger(Logger &&) = delete;
    Logger &operator=(Logger &&) = delete;

    void start() noexcept;
    void request_stop() noexcept;

private:
    friend class LoggerRegistry;

    static constexpr size_t kModulesSize = 4;
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
    };

    LoggerConfig m_config;

    std::atomic<bool> m_running;

    FileDescriptor m_epoll;
    FileDescriptor m_notify;

    std::array<Module, kModulesSize> m_modules;
    size_t m_registered;

    LoggerNode create_node(std::string_view name);

    void drain_modules() noexcept; 
    size_t drain_module(Module &module) noexcept; 

    void write_record(Module &module, const LogRecord &record) noexcept;
    void write_dropped(const Module &module, size_t dropped) noexcept;

    void rotate(Module &module, nanoseconds timestamp) noexcept;
    void append(const Module &module, const char *data, size_t size) noexcept;
};

} // namespace nlx::logging
#endif // !NLX_LOGGING_LOGGER_H
