#include "core/config.h"
#include "core/string.h"

#include <stdexcept>

namespace nlx
{

Config::Config() :
    logger()
{
    detail::str_assign(logger.directory, "logs"); 
    logger.rotation_minutes = 1440;
    logger.flush_ms = 250;
}

Config Config::load(std::string_view path)
{
    throw std::logic_error("is not implemented");
}

} // namespace nlx
