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
    int liquidity_window_days;
};

struct Config
{
    LoggerConfig logger;
    OkxConfig okx;
    StrategyConfig strategy;

    Config();

    [[nodiscard]] static Config load(const char *path);
};

} // namespace nlx
#endif // !NLX_CORE_CONFIG_H
