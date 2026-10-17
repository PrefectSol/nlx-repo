#include "parsing/json_reader.h"

#include <simdjson.h>

#include <charconv>

namespace nlx::parsing::detail
{

namespace
{

static_assert(json_padding_size >= simdjson::SIMDJSON_PADDING);

template<typename T>
bool to_number(std::string_view text, T &value) noexcept
{
    const char *end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);

    return result.ec == std::errc() && result.ptr == end;
}

simdjson::dom::parser &parser() noexcept
{
    thread_local simdjson::dom::parser instance;
    return instance;
}

bool parse(std::string_view json, int &code, simdjson::dom::array &data) noexcept
{
    simdjson::dom::element root;

    const simdjson::error_code result = parser().parse(json.data(), json.size(), false).get(root); 
    if (result != simdjson::SUCCESS)
    {
        return false;
    }

    std::string_view text;
    if (root["code"].get(text) != simdjson::SUCCESS || !to_number(text, code))
    {
        return false;
    }

    if (code != 0)
    {
        return true;
    }

    return root["data"].get(data) == simdjson::SUCCESS;
}

} // namespace

bool okx_parse_code(std::string_view json, int &code) noexcept
{
    simdjson::dom::array data;
    return parse(json, code, data);
}

bool okx_parse_time(std::string_view json, int &code, milliseconds &time) noexcept
{
    simdjson::dom::array data;
    if (!parse(json, code, data))
    {
        return false;
    }

    if (code != 0)
    {
        return true;
    }

    std::string_view text;
    int64_t value = 0;
    if (data.at(0)["ts"].get(text) != simdjson::SUCCESS || !to_number(text, value))
    {
        return false;
    }

    time = milliseconds(value);

    return true;
}

bool okx_parse_account_config(std::string_view json, int &code, uint64_t &uid, int &mode, bool &hedge, bool &trade) noexcept
{
    simdjson::dom::array data;
    if (!parse(json, code, data))
    {
        return false;
    }

    if (code != 0)
    {
        return true;
    }

    simdjson::dom::element config;
    std::string_view user;
    std::string_view level;
    std::string_view position;
    std::string_view permissions;

    const bool parsed = data.at(0).get(config) == simdjson::SUCCESS &&
                        config["uid"].get(user) == simdjson::SUCCESS &&
                        config["acctLv"].get(level) == simdjson::SUCCESS &&
                        config["posMode"].get(position) == simdjson::SUCCESS &&
                        config["perm"].get(permissions) == simdjson::SUCCESS;

    if (!parsed || !to_number(user, uid) || !to_number(level, mode))
    {
        return false;
    }

    hedge = position == "long_short_mode";
    trade = permissions.find("trade") != std::string_view::npos;

    return true;
}

bool okx_parse_balance(std::string_view json, int &code, std::string_view currency, double &available) noexcept
{
    simdjson::dom::array data;
    if (!parse(json, code, data))
    {
        return false;
    }

    if (code != 0)
    {
        return true;
    }

    simdjson::dom::array assets;
    if (data.at(0)["details"].get(assets) != simdjson::SUCCESS)
    {
        return false;
    }

    available = 0.0;
    for (const simdjson::dom::element asset : assets)
    {
        std::string_view name;
        if (asset["ccy"].get(name) != simdjson::SUCCESS)
        {
            return false;
        }

        if (name != currency)
        {
            continue;
        }

        std::string_view text;
        return asset["availEq"].get(text) == simdjson::SUCCESS && to_number(text, available);
    }

    return true;
}

bool okx_parse_positions(std::string_view json, int &code, size_t &count) noexcept
{
    simdjson::dom::array data;
    if (!parse(json, code, data))
    {
        return false;
    }

    if (code != 0)
    {
        return true;
    }

    count = 0;
    for (const simdjson::dom::element position : data)
    {
        std::string_view text;
        double size = 0.0;
        if (position["pos"].get(text) != simdjson::SUCCESS || !to_number(text, size))
        {
            return false;
        }

        if (size != 0.0)
        {
            ++count;
        }
    }

    return true;
}

} // namespace nlx::parsing::detail
