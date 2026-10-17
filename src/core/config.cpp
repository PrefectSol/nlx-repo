#include "core/config.h"
#include "core/string.h"

#include <stdexcept>

namespace nlx
{

Config::Config() :
    logger(), okx() 
{
    detail::str_assign(logger.directory, "logs"); 
    logger.rotation_minutes = 1440;
    logger.flush_ms = 250;

    okx.simulated = true;
}

Config Config::load(const char *path)
{
    throw std::logic_error("is not implemented");
}

} // namespace nlx
