#include "core/credentials.h"
#include "core/string.h"
#include "parsing/env_reader.h"

#include <stdexcept>

namespace nlx
{

Credentials Credentials::load(const char *path)
{
    const parsing::EnvReader reader(path);

    Credentials credentials{};
    bool result = string::safe_strcpy(credentials.okx_api_key, reader.get("OKX_API_KEY"));
    if (!result)
    {
        throw std::invalid_argument("OKX_API_KEY is too long");
    }

    result = string::safe_strcpy(credentials.okx_api_secret, reader.get("OKX_API_SECRET"));
    if (!result)
    {
        throw std::invalid_argument("OKX_API_SECRET is too long");
    }

    result = string::safe_strcpy(credentials.okx_api_passphrase, reader.get("OKX_API_PASSPHRASE"));
    if (!result)
    {
        throw std::invalid_argument("OKX_API_PASSPHRASE is too long");
    }

    return credentials;
}

} // namespace nlx
