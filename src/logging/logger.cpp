#include "logging/logger.h"
#include "core/fatal.h"
#include "core/file.h"
#include "core/file_descriptor.h"
#include "core/string.h"
#include "core/system_error.h"
#include "core/time.h"
#include "logging/logger_node.h"

#include <linux/limits.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <format>
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
        case LogLevel::Metric:
            return "[METRIC] ";
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

Logger::Logger() :
    m_initialized(false),
    m_epoll(),
    m_notify(),
    m_stop(),
    m_modules(),
    m_registered(0),
    m_self(),
    m_loop(),
    m_write(),
    m_lag(),
    m_bytes(0),
    m_reported()
{
    nlx::detail::str_assign(m_self.name, std::string_view("logger"));

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
    if (!file::mkdir_p(kDirectory))
    {
        throw SystemError("failed to create directory");
    }
    
    if (!file::writable(kDirectory))
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

    m_reported = time::now_monotonic();
    m_loop.reset(m_reported);

    epoll_event event{};
    while (true)
    {
        m_loop.sleep(time::now_monotonic());
        const size_t count = nlx::detail::epoll_wait(m_epoll, { &event, 1 }, kFlushMilliseconds);
        m_loop.wake(time::now_monotonic());

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

        const nanoseconds now = time::now_monotonic();
        if (now - m_reported >= milliseconds(kMetricsMilliseconds))
        {
            report(now);
        }
    }

    drain_modules();
}

void Logger::request_stop() noexcept
{
    nlx::detail::notify(m_stop);
}

std::string_view Logger::get_directory() const noexcept
{
    return kDirectory;
}

size_t Logger::get_rotation_minutes() const noexcept
{
    return kRotationMinutes;
}

size_t Logger::get_flush_milliseconds() const noexcept
{
    return kFlushMilliseconds;
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

    module.records += count;
    module.peak = std::max(module.peak, count);

    const size_t dropped = module.channel.dropped.exchange(0, std::memory_order_relaxed);
    if (dropped != 0) [[unlikely]]
    {
        module.dropped += dropped;
        write_dropped(module, dropped);
    }

    return count;
}

void Logger::write_record(Module &module, const LogRecord &record) noexcept
{
    const nanoseconds interval = module.rotation + minutes(kRotationMinutes);
    if (record.timestamp >= interval) [[unlikely]]
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

    m_lag.record(time::now() - record.timestamp);
    write(module, line.finish());
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

void Logger::write_text(Module &module, LogLevel level, std::string_view text) noexcept
{
    const nanoseconds now = time::now();

    const nanoseconds interval = module.rotation + minutes(kRotationMinutes);
    if (now >= interval) [[unlikely]]
    {
        rotate(module, now);
    }

    char date_buffer[kDateSize];
    const std::string_view date = nlx::detail::format_time(date_buffer, kDateFormat, now);

    string::BoundedLine<kLineSize> line;
    line.append("[");
    line.append(date);
    line.append("] ");
    line.append(level_nameblock(level));
    line.append(" ");
    line.append(text);

    write(module, line.finish());
}

void Logger::write(Module &module, std::string_view text) noexcept
{
    const nanoseconds begin = time::now_monotonic();
    if (!nlx::detail::writeTo(module.file, text.data(), text.size())) [[unlikely]]
    {
        NLX_FATAL("nlx::logging::Logger::write() detail::writeTo failed");
    }

    m_write.record(time::now_monotonic() - begin);
    m_bytes += text.size();
}

void Logger::report(nanoseconds now) noexcept
{
    char buffer[kLineSize];

    const auto total = std::format_to_n(buffer, sizeof(buffer),
                                        R"(logger metrics: utilization = "{}" %, wakeups = "{}", lines = "{}", bytes = "{}", write p50 = "{}" us, p99 = "{}" us, max = "{}" us, )"
                                        R"(lag p50 = "{}" us, p99 = "{}" us, max = "{}" us)",
                                        m_loop.utilization(now), m_loop.wakeups(), m_write.count(), m_bytes, m_write.percentile(0.5),
                                        m_write.percentile(0.99), m_write.max(), m_lag.percentile(0.5), m_lag.percentile(0.99), m_lag.max());
    write_text(m_self, LogLevel::Metric, std::string_view(buffer, std::min(static_cast<size_t>(total.size), sizeof(buffer))));

    for (size_t i = 0; i < m_registered; ++i)
    {
        Module &module = m_modules[i];

        const auto line = std::format_to_n(buffer, sizeof(buffer), R"(logger module metrics: module = "{}", records = "{}", dropped = "{}", queue peak = "{}" of "{}")",
                                           std::string_view(module.name), module.records, module.dropped, module.peak, LoggerChannel::kQueueCapacity);
        write_text(m_self, LogLevel::Metric, std::string_view(buffer, std::min(static_cast<size_t>(line.size), sizeof(buffer))));

        module.records = 0;
        module.dropped = 0;
        module.peak = 0;
    }

    m_loop.reset(now);
    m_write.reset();
    m_lag.reset();
    m_bytes = 0;
    m_reported = now;
}

void Logger::rotate(Module &module, nanoseconds timestamp) noexcept
{
    module.rotation = timestamp - timestamp % minutes(kRotationMinutes);

    char date_buffer[kDateSize]; 
    const std::string_view date = nlx::detail::format_time(date_buffer, kRotationFormat, module.rotation);
    
    char path[PATH_MAX];
    const auto result = std::format_to_n(path, sizeof(path) - 1, "{}/{}_{}{}", kDirectory, date, module.name, kFileExtension);
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
