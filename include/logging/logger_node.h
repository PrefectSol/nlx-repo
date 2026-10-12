#ifndef NLX_LOGGING_LOGGER_NODE_H
#define NLX_LOGGING_LOGGER_NODE_H

#include "core/file_descriptor.h"
#include "core/spsc_queue.h"
#include "core/time.h"
#include "core/string.h"

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>
#include <cassert>

namespace nlx::logging
{

enum class LogLevel : uint8_t
{
    Debug,
    Info,
    Warning,
    Error
};

struct LogArgument
{
    enum class Type : uint8_t
    {
        None,
        Integer,
        UInteger,
        Floating,
        Boolean,
        String
    };

    union Value
    {
        int64_t as_int;
        uint64_t as_uint;
        double as_double;
        bool as_bool;

        struct
        {
            uint8_t offset;
            uint8_t length;
            bool truncated;
        } as_string;
    };
};

struct alignas(64) LogRecord
{
    static constexpr size_t kMaxArgs = 8;
    static constexpr size_t kBufferSize = 224;
    static constexpr size_t kFormatCapacity = 1024;

    nanoseconds timestamp;

    const char *format;
    uint32_t size;
    
    uint8_t nargs;
    uint8_t used;
    
    LogLevel level;

    LogArgument::Value values[kMaxArgs];
    LogArgument::Type types[kMaxArgs];

    char buffer[kBufferSize];
};

static_assert(std::is_trivially_copyable_v<LogRecord>);
static_assert(LogRecord::kBufferSize <= UINT8_MAX);
static_assert(LogRecord::kMaxArgs <= UINT8_MAX);

struct LoggerChannel
{
    static constexpr size_t kQueueCapacity = 128;

    SpscQueue<LogRecord, kQueueCapacity> queue;
    std::atomic<size_t> dropped{0};
};

template<typename ...Args>
using FormatString = string::BasicFormatString<LogRecord::kFormatCapacity, std::type_identity_t<Args>...>;

class LoggerNode
{
public:
    LoggerNode() noexcept;
    explicit LoggerNode(LoggerChannel &channel, const FileDescriptor &notify) noexcept;

    LoggerNode(LoggerChannel &channel, const FileDescriptor &&notify) = delete;
    
    ~LoggerNode() noexcept;

    LoggerNode(const LoggerNode &) = delete;
    LoggerNode &operator=(const LoggerNode &) = delete;

    LoggerNode(LoggerNode &&other) noexcept;
    LoggerNode &operator=(LoggerNode &&other) noexcept;

    template<LogLevel Level, typename ...Args>
    void log(FormatString<Args...> format, Args &&...args) noexcept;

    template<typename ...Args>
    void log(FormatString<Args...> format, Args &&...args) noexcept;

    void flush() noexcept;

private:
    static constexpr LogLevel kMinLevel = LogLevel::Debug;

    const FileDescriptor *m_notify;
    LoggerChannel *m_channel;
};

namespace detail
{

template<std::integral T>
constexpr void make_arg(LogRecord &record, size_t index, T value) noexcept;

template<std::floating_point T>
constexpr void make_arg(LogRecord &record, size_t index, T value) noexcept;

constexpr void make_arg(LogRecord &record, size_t index, std::string_view value) noexcept;
constexpr void make_arg(LogRecord &record, size_t index, const char *value) noexcept;
constexpr void make_arg(LogRecord &record, size_t index, char value) noexcept;

} // namespace nlx::logging::detail
} // namespace nlx::logging

namespace nlx::logging
{

template<LogLevel Level, typename ...Args>
void LoggerNode::log(FormatString<Args...> format, Args &&...args) noexcept
{
    static_assert(sizeof...(Args) <= LogRecord::kMaxArgs, "too many log arguments");

    if constexpr (Level < kMinLevel)
    {
        return;
    }

    if (m_channel == nullptr) [[unlikely]]
    {
        return;
    }

    LogRecord *record = m_channel->queue.claim();
    if (record == nullptr) [[unlikely]]
    {
        m_channel->dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const std::string_view view = format.get();

    record->timestamp = time::now();
    record->format = view.data();
    record->size = static_cast<uint32_t>(view.size());
    record->level = Level;
    record->nargs = static_cast<uint8_t>(sizeof...(Args));
    record->used = 0;

    [[maybe_unused]] size_t idx = 0;
    (detail::make_arg(*record, idx++, std::forward<Args>(args)), ...);

    [[maybe_unused]] const bool committed = m_channel->queue.commit();
    assert(committed);
}

template<typename ...Args>
void LoggerNode::log(FormatString<Args...> format, Args &&...args) noexcept
{
    log<LogLevel::Info>(format, std::forward<Args>(args)...);
}

namespace detail
{

template<std::integral T>
constexpr void make_arg(LogRecord &record, size_t index, T value) noexcept
{
    if constexpr (std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        record.types[index] = LogArgument::Type::Boolean;
        record.values[index].as_bool = value;
    }
    else if constexpr (std::is_signed_v<T>)
    {
        record.types[index] = LogArgument::Type::Integer;
        record.values[index].as_int = static_cast<int64_t>(value);
    }
    else
    {
        record.types[index] = LogArgument::Type::UInteger;
        record.values[index].as_uint = static_cast<uint64_t>(value);
    }
}

template<std::floating_point T>
constexpr void make_arg(LogRecord &record, size_t index, T value) noexcept
{
    record.types[index] = LogArgument::Type::Floating;
    record.values[index].as_double = static_cast<double>(value);
}

constexpr void make_arg(LogRecord &record, size_t index, std::string_view value) noexcept
{
    const size_t available = LogRecord::kBufferSize - record.used;
    const size_t length = std::min(value.size(), available);

    std::copy_n(value.data(), length, record.buffer + record.used);

    record.types[index] = LogArgument::Type::String;
    record.values[index].as_string.offset = record.used;
    record.values[index].as_string.length = static_cast<uint8_t>(length);
    record.values[index].as_string.truncated = (length != value.size());
    record.used = static_cast<uint8_t>(record.used + length);
}

constexpr void make_arg(LogRecord &record, size_t index, const char *value) noexcept
{
    make_arg(record, index, value != nullptr ? std::string_view(value) : std::string_view("(null)"));
}

constexpr void make_arg(LogRecord &record, size_t index, char value) noexcept
{
    make_arg(record, index, std::string_view(&value, 1));
}

} // namespace nlx::logging::detail
} // namespace nlx::logging
#endif // !NLX_LOGGING_LOGGER_NODE_H
