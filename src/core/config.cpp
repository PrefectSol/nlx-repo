#include "core/config.h"
#include "core/string.h"

#include <stdexcept>

namespace nlx
{

Config::Config() :
    logger(), okx(), strategy()
{
    detail::str_assign(logger.directory, "logs");
    logger.rotation_minutes = 1440;
    logger.flush_ms = 250;

    detail::str_assign(okx.url, "https://openapi.okx.com");
    okx.simulated = true;

    strategy.instruments_refresh_minutes = 1440;
    strategy.liquidity_window_days = 14;
}

Config Config::load(const char *path)
{
    throw std::logic_error("is not implemented");
}

} // namespace nlx
