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

constexpr std::string_view level_name(LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::Debug:
            return "[DEBUG]   ";
        case LogLevel::Info:
            return "[INFO]    ";
        case LogLevel::Warning:
            return "[WARNING] ";
        case LogLevel::Error:
            return "[ERROR]   ";
    }

    __builtin_unreachable();
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

template<size_t N, typename T>
    requires std::integral<T> || std::floating_point<T>
std::string_view num2string(char (&buffer)[N], T value) noexcept
{
    const auto result = std::to_chars(buffer, buffer + N, value);
    if (result.ec != std::errc()) [[unlikely]]
    {
        return {};
    }

    return std::string_view(buffer, static_cast<size_t>(result.ptr - buffer));
}

} // namespace

Logger::Logger(LoggerConfig config) :
    m_config(std::move(config)),
    m_running(true),
    m_epoll(),
    m_notify(),
    m_modules(),
    m_registered(0)
{
    if (m_config.directory[0] == '\0' ||
        m_config.rotation_minutes <= 0 || 
        m_config.flush_ms <= 0)
    {
        throw std::invalid_argument("invalid configuration");
    }

    if (!file::mkdir_p(m_config.directory))
    {
        throw std::runtime_error("failed to create directory");
    }
    
    if (!file::writable(m_config.directory))
    {
        throw SystemError("directory is not writable");
    }

    m_epoll = FileDescriptor::make_epoll();
    m_notify = FileDescriptor::make_eventfd(true);

    nlx::detail::epoll_add(m_epoll, m_notify);
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

void Logger::run() noexcept
{
    epoll_event event{};
    while (m_running.load(std::memory_order_acquire))
    {
        const size_t count = nlx::detail::epoll_wait(m_epoll, { &event, 1 }, m_config.flush_ms);
        if (count != 0)
        {
            [[maybe_unused]] const uint64_t result = nlx::detail::drain(m_notify);
        }

        drain_modules();
    }

    drain_modules();
}

void Logger::wake() noexcept
{
    m_running.store(false, std::memory_order_release);
    nlx::detail::notify(m_notify);
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

    string::BoundedLine<kLineSize> line;
    
    constexpr size_t datesize = nlx::detail::length_dateformat(kDateFormat) + 1;
    char datebuffer[datesize];

    line.append("[");
    line.append(nlx::detail::format_time(datebuffer, kDateFormat, record.timestamp));
    line.append("] ");
    line.append(level_name(record.level));

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
            const LogArgument::Value &value = record.values[index];

            line.append("\"");

            char numbuffer[kNumSize];
            switch (record.types[index])
            {
                case LogArgument::Type::Integer:
                    line.append(num2string(numbuffer, value.as_int));
                    break;
                case LogArgument::Type::UInteger:
                    line.append(num2string(numbuffer, value.as_uint));
                    break;
                case LogArgument::Type::Floating:
                    line.append(num2string(numbuffer, value.as_double));
                    break;
                case LogArgument::Type::Boolean:
                    line.append(value.as_bool ? "true" : "false");
                    break;
                case LogArgument::Type::String:
                    line.append(std::string_view(record.buffer + value.as_string.offset, value.as_string.length));
                    if (value.as_string.truncated) [[unlikely]]
                    {
                        line.append(kTruncatedMarker);
                    }

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
    append(module, text.data(), text.size());
}

void Logger::write_dropped(const Module &module, size_t dropped) noexcept
{
    constexpr size_t size = nlx::detail::length_dateformat(kDateFormat);
    constexpr std::array<char, size> placeholder = []()  {
        std::array<char, size> array;
        array.fill(kPlaceholder);
        return array;
    }();
    
    char numbuffer[kNumSize];

    string::BoundedLine<kLineSize> line;
    
    line.append("[");
    line.append(std::string_view(placeholder.data(), size));
    line.append("]");
    line.append(level_name(LogLevel::Warning));
    line.append("dropped records: ");
    line.append(num2string(numbuffer, dropped));

    const std::string_view text = line.finish();
    append(module, text.data(), text.size());
}

void Logger::rotate(Module &module, nanoseconds timestamp) noexcept
{
    module.rotation = timestamp - timestamp % minutes(m_config.rotation_minutes);

    constexpr size_t size = nlx::detail::length_dateformat(kRotationFormat) + 1;
    char buffer[size]; 
    
    const std::string_view date = nlx::detail::format_time(buffer, kRotationFormat, module.rotation);
    
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

void Logger::append(const Module &module, const char *data, size_t size) noexcept
{
    if (!nlx::detail::writeTo(module.file, data, size)) [[unlikely]]
    {
        NLX_FATAL("nlx::logging::Logger::append() detail::writeTo failed");
    }

    if (m_config.console)
    {
        size_t offset = 0;
        while (offset < size)
        {
            const ssize_t result = ::write(STDOUT_FILENO, data + offset, size - offset);
            if (result < 0) [[unlikely]]
            {
                if (errno == EINTR)
                {
                    continue;
                }

                break; 
            }

            offset += static_cast<size_t>(result);
        }
    }
}

} // namespace nlx::logging
