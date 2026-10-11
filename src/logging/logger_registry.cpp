#include "logging/logger_registry.h"

namespace nlx::logging
{

LoggerRegistry::LoggerRegistry(Logger &logger) noexcept :
    m_logger(logger)
{

}

LoggerNode LoggerRegistry::create_node(std::string_view name)
{
    return m_logger.create_node(name);
}

} // namespace nlx::logging

