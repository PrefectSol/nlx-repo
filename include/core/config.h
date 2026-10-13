#ifndef NLX_CORE_CONFIG_H
#define NLX_CORE_CONFIG_H

#include <string_view>

namespace nlx
{

struct LoggerConfig
{
    char directory[128];
    int rotation_minutes;
    int flush_ms;
};

class Config
{
public:
    LoggerConfig logger;

    Config();
    
    [[nodiscard]] static Config load(std::string_view path);
};

} // namespace nlx
#endif // !NLX_CORE_CONFIG_H
