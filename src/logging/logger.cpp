#include "logging/logger.h"
#include "logging/logger_node.h"
#include "core/config.h"
#include "core/file.h"
#include "core/string.h"
#include "core/system_error.h"
#include "core/fatal.h"
#include "core/file_descriptor.h"
#include "core/time.h"

#include <linux/limits.h>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace nlx::logging
{

namespace 
{

constexpr std::string_view level_nameblock(LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::Debug:
            return "[DEBUG]  ";
        case LogLevel::Info:
            return "[INFO]   ";
        case LogLevel::Warning:
            return "[WARNING]";
        case LogLevel::Error:
            return "[ERROR]  ";
    }

    std::unreachable();
}

constexpr bool validate_name(std::string_view name) noexcept
{
    if (name.empty())
    {
        return false;
    }

    for (char ch : name)
    {
        const bool is_alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        const bool is_digit = ch >= '0' && ch <= '9';

        if (!is_alpha && !is_digit && ch != '_' && ch != '-')
        {
            return false;
        }
    }

    return true;
}

} // namespace

Logger::Logger(const LoggerConfig &config) :
    m_config(config),
    m_initialized(false),
    m_epoll(),
    m_notify(),
    m_stop(),
    m_modules(),
    m_registered(0)
{
    if (m_config.directory[0] == '\0' ||
        m_config.rotation_minutes <= 0 || 
        m_config.flush_ms <= 0)
    {
        throw std::invalid_argument("invalid configuration");
    }

    m_epoll = FileDescriptor::make_epoll();
    m_notify = FileDescriptor::make_eventfd(true);
    m_stop = FileDescriptor::make_eventfd(true);

    nlx::detail::epoll_add(m_epoll, m_notify);
    nlx::detail::epoll_add(m_epoll, m_stop);
}

LoggerNode Logger::create_node(std::string_view name)
{
    if (m_registered >= kModulesSize)
    {
        throw std::out_of_range("module limit exceeded");
    }

    const size_t length = name.size();
    if (!validate_name(name) || length >= kNameSize)
    {
        throw std::invalid_argument("invalid module name");
    }

    for (size_t i = 0; i < m_registered; ++i)
    {
        if (name == std::string_view(m_modules[i].name))
        {
            throw std::logic_error("name already registered");
        }
    }

    Module &module = m_modules[m_registered];
    nlx::detail::str_assign(module.name, name);

    ++m_registered;

    return LoggerNode(module.channel, m_notify);
}

void Logger::initialize()
{
    if (!file::mkdir_p(m_config.directory))
    {
        throw SystemError("failed to create directory");
    }
    
    if (!file::writable(m_config.directory))
    {
        throw SystemError("directory is not writable");
    }

    m_initialized.store(true, std::memory_order_release);
}

void Logger::start() noexcept
{
    if (!m_initialized.load(std::memory_order_acquire)) [[unlikely]]
    {
        NLX_FATAL("nlx::logging::Logger::start() called before initialize()");
    }

    epoll_event event{};
    while (true)
    {
        const size_t count = nlx::detail::epoll_wait(m_epoll, { &event, 1 }, m_config.flush_ms);
        if (count != 0)
        {
            if (event.data.fd == m_stop)
            {
                [[maybe_unused]] const uint64_t result = nlx::detail::drain(m_stop);
                break;
            }

            [[maybe_unused]] const uint64_t result = nlx::detail::drain(m_notify);
        }

        drain_modules();
    }

    drain_modules();
}

void Logger::request_stop() noexcept
{
    nlx::detail::notify(m_stop);
}

void Logger::drain_modules() noexcept
{
    size_t processed;
    do
    {
        processed = 0;
        for (size_t i = 0; i < m_registered; ++i)
        {
            processed += drain_module(m_modules[i]);
        }
    }
    while (processed != 0);
}

size_t Logger::drain_module(Module &module) noexcept
{
    size_t count = 0;
    for (; count < LoggerChannel::kQueueCapacity; ++count)
    {
        const LogRecord *record = module.channel.queue.front();
        if (record == nullptr)
        {
            break;
        }
        
        write_record(module, *record);

        [[maybe_unused]] const bool consumed = module.channel.queue.consume();
        assert(consumed);
    }

    const size_t dropped = module.channel.dropped.exchange(0, std::memory_order_relaxed);
    if (dropped != 0) [[unlikely]]
    {
        write_dropped(module, dropped);
    }

    return count;
}

void Logger::write_record(Module &module, const LogRecord &record) noexcept
{
    if (record.timestamp >= module.rotation + minutes(m_config.rotation_minutes)) [[unlikely]]
    {
        rotate(module, record.timestamp);
    }

    char date_buffer[kDateSize];
    const std::string_view date = nlx::detail::format_time(date_buffer, kDateFormat, record.timestamp);

    string::BoundedLine<kLineSize> line;
    line.append("[");
    line.append(date);
    line.append("] ");
    line.append(level_nameblock(record.level));
    line.append(" ");

    const std::string_view format(record.format, record.size);
    const size_t size = format.size();

    size_t index = 0;
    size_t begin = 0;
    for (size_t i = 0; i < size; ++i)
    {
        const char ch = format[i];
        if (ch != '{' && ch != '}')
        {
            continue;
        }

        line.append(format.substr(begin, i - begin));

        if (ch == '{' && format[i + 1] == '}')
        {
            line.append("\"");

            const LogArgument::Value &value = record.values[index];
            switch (record.types[index])
            {
                case LogArgument::Type::Integer:
                    line.append( value.as_int);
                    break;

                case LogArgument::Type::UInteger:
                    line.append(value.as_uint);
                    break;

                case LogArgument::Type::Floating:
                    line.append(value.as_double);
                    break;

                case LogArgument::Type::Boolean:
                    line.append(value.as_bool);
                    break;

                case LogArgument::Type::String:
                    line.append(std::string_view(record.buffer + value.as_string.offset, value.as_string.length));
                    line.append((value.as_string.truncated ? kTruncatedMarker : ""));
                    break;

                case LogArgument::Type::None:
                    break;
            }

            line.append("\"");
            ++index;
        }
        else
        {
            line.append(format.substr(i, 1));
        }

        ++i;
        begin = i + 1;
    }

    line.append(format.substr(begin));

    const std::string_view text = line.finish();
    if (!nlx::detail::writeTo(module.file, text.data(), text.size())) [[unlikely]]
    {
        NLX_FATAL("nlx::logging::Logger::write_record() detail::writeTo failed");
    }
}

void Logger::write_dropped(const Module &module, size_t dropped) noexcept
{
    char date_buffer[kDateSize];
    const std::string_view date = nlx::detail::format_time(date_buffer, kDateFormat, time::now());
    
    string::BoundedLine<kLineSize> line;
    line.append("[");
    line.append(date);
    line.append("] ");
    line.append(level_nameblock(LogLevel::Warning));
    line.append(" dropped records: ");
    line.append(dropped);

    const std::string_view text = line.finish();
    if (!nlx::detail::writeTo(module.file, text.data(), text.size())) [[unlikely]]
    {
        NLX_FATAL("nlx::logging::Logger::write_dropped() detail::writeTo failed");
    }
}

void Logger::rotate(Module &module, nanoseconds timestamp) noexcept
{
    module.rotation = timestamp - timestamp % minutes(m_config.rotation_minutes);

    char date_buffer[kDateSize]; 
    const std::string_view date = nlx::detail::format_time(date_buffer, kRotationFormat, module.rotation);
    
    char path[PATH_MAX];
    const auto result = std::format_to_n(path, sizeof(path) - 1, "{}/{}_{}{}", m_config.directory, date, module.name, kFileExtension);
    if (static_cast<size_t>(result.size) >= sizeof(path)) [[unlikely]]
    {
        NLX_FATAL("nlx::logging::Logger::rotate() path is too long");
    }

    *result.out = '\0';

    module.file = FileDescriptor::open_append(path);
    if (!module.file.valid()) [[unlikely]]
    {
        NLX_FATAL("nlx::logging::Logger::rotate() FileDescriptor::open_append failed");
    }
}

} // namespace nlx::logging
