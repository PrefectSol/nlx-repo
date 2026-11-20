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

    const bool demo = reader.contains("OKX_DEMO");
    credentials.okx_demo = demo && (reader.get("OKX_DEMO") == "1" || string::iequals(reader.get("OKX_DEMO"), "true"));

    const bool key = string::safe_strcpy(credentials.okx_api_key, reader.get(credentials.okx_demo ? "DEMO_OKX_API_KEY" : "OKX_API_KEY"));
    if (!key)
    {
        throw std::invalid_argument("okx api key is too long");
    }

    const bool secret = string::safe_strcpy(credentials.okx_api_secret, reader.get(credentials.okx_demo ? "DEMO_OKX_API_SECRET" : "OKX_API_SECRET"));
    if (!secret)
    {
        throw std::invalid_argument("okx api secret is too long");
    }

    const bool passphrase = string::safe_strcpy(credentials.okx_api_passphrase, reader.get(credentials.okx_demo ? "DEMO_OKX_API_PASSPHRASE" : "OKX_API_PASSPHRASE"));
    if (!passphrase)
    {
        throw std::invalid_argument("okx api passphrase is too long");
    }

    const bool model = !reader.contains("NLX_MODEL_KEY") || string::safe_strcpy(credentials.model_key, reader.get("NLX_MODEL_KEY"));
    if (!model)
    {
        throw std::invalid_argument("NLX_MODEL_KEY is too long");
    }

    return credentials;
}

} // namespace nlx
