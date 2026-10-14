#ifndef NLX_CORE_CONFIG_H
#define NLX_CORE_CONFIG_H

#include <cstddef>

namespace nlx
{

struct LoggerConfig
{
    static constexpr size_t kPathCapacity = 128;

    char directory[kPathCapacity];
    int rotation_minutes;
    int flush_ms;
};

struct Config
{
    LoggerConfig logger;

    Config();

    [[nodiscard]] static Config load(const char *path);
};

} // namespace nlx
#endif // !NLX_CORE_CONFIG_H
