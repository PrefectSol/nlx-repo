#include "core/credentials.h"
#include "core/string.h"
#include "parsing/env_reader.h"

#include <stdexcept>

namespace nlx
{

Credentials Credentials::load(const char *path)
{
    Credentials credentials{};
    parsing::EnvReader reader(path);

    bool result = string::safe_strcpy(credentials.api_key, reader.get("API_KEY"));
    if (!result)
    {
        throw std::invalid_argument("API_KEY is too long");
    }

    result = string::safe_strcpy(credentials.api_secret, reader.get("API_SECRET"));
    if (!result)
    {
        throw std::invalid_argument("API_SECRET is too long");
    }

    return credentials;
}

} // namespace nlx
