#include "core/config.h"
#include "core/string.h"

namespace nlx
{

Config Config::load()
{
    Config config{};

    detail::str_assign(config.logger.directory, "logs");
    config.logger.rotation_minutes = 1440;
    config.logger.flush_ms = 250;

    detail::str_assign(config.okx.url, "https://openapi.okx.com");
    config.okx.simulated = true;
    detail::str_assign(config.okx.websocket_url, config.okx.simulated ? "wss://wspap.okx.com" : "wss://ws.okx.com");

    config.strategy.instruments_refresh_minutes = 1440;

    return config;
}

} // namespace nlx
