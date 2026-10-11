#ifndef NLX_LOGGING_LOGGER_REGISTRY_H
#define NLX_LOGGING_LOGGER_REGISTRY_H

#include "logging/logger.h"

#include <string_view>

namespace nlx::logging
{

class LoggerRegistry
{
public:
    explicit LoggerRegistry(Logger &logger) noexcept;

    LoggerRegistry(const LoggerRegistry &) = delete;
    LoggerRegistry &operator=(const LoggerRegistry &) = delete;
    LoggerRegistry(LoggerRegistry &&) = delete;
    LoggerRegistry &operator=(LoggerRegistry &&) = delete;

    [[nodiscard]] LoggerNode create_node(std::string_view name);

private:
    Logger &m_logger;
};

} // namespace nlx::logging
#endif // !NLX_LOGGING_LOGGER_REGISTRY_H
