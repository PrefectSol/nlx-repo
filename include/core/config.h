#ifndef NLX_CORE_CONFIG_H
#define NLX_CORE_CONFIG_H

#include <cstddef>

namespace nlx
{

struct LoggerConfig
{
    static constexpr size_t kCapacity = 128;

    char directory[kCapacity];
    int rotation_minutes;
    int flush_ms;
};

struct OkxConfig
{
    static constexpr size_t kCapacity = 128;

    char url[kCapacity];
    bool simulated;
};

struct StrategyConfig
{
    int instruments_refresh_minutes;
};

struct Config
{
    LoggerConfig logger;
    OkxConfig okx;
    StrategyConfig strategy;

    [[nodiscard]] static Config load();
};

} // namespace nlx
#endif // !NLX_CORE_CONFIG_H
