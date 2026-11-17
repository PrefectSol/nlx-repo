#include "parsing/json_reader.h"
#include "core/string.h"

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

bool consume_prefix(std::string_view &text, std::string_view prefix) noexcept
{
    const bool matched = text.starts_with(prefix);
    if (matched)
    {
        text.remove_prefix(prefix.size());
    }

    return matched;
}

bool consume_suffix(std::string_view &text, std::string_view suffix) noexcept
{
    const bool matched = text.ends_with(suffix);
    if (matched)
    {
        text.remove_suffix(suffix.size());
    }

    return matched;
}

template<typename T>
bool read_field(std::string_view &text, T &value) noexcept
{
    const char *const end = text.data() + text.size();

    const std::from_chars_result result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc())
    {
        return false;
    }

    text.remove_prefix(static_cast<size_t>(result.ptr - text.data()));

    return text.empty() || consume_prefix(text, R"(",")");
}

bool skip_field(std::string_view &text) noexcept
{
    const size_t quote = text.find('"');
    if (quote == std::string_view::npos)
    {
        return false;
    }

    text.remove_prefix(quote);

    return consume_prefix(text, R"(",")");
}

simdjson::dom::parser &parser() noexcept
{
    thread_local simdjson::dom::parser instance;
    return instance;
}

bool parse(std::string_view json, int &code, simdjson::dom::array &data) noexcept
{
    simdjson::dom::element root;
    bool parsed = parser().parse(json.data(), json.size(), false).get(root) == simdjson::SUCCESS;

    std::string_view text;
    parsed = parsed && root["code"].get(text) == simdjson::SUCCESS;

    if (!parsed || !to_number(text, code))
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

    const bool result = parse(json, code, data);
    if (!result)
    {
        return false;
    }

    if (code != 0)
    {
        return true;
    }

    std::string_view text;
    int64_t value = 0;

    const bool parsed = data.at(0)["ts"].get(text) == simdjson::SUCCESS;
    if (!parsed || !to_number(text, value))
    {
        return false;
    }

    time = milliseconds(value);

    return true;
}

bool okx_parse_account_config(std::string_view json, int &code, uint64_t &uid, int &mode, bool &hedge, bool &trade) noexcept
{
    simdjson::dom::array data;
    
    const bool result = parse(json, code, data);
    if (!result)
    {
        return false;
    }

    if (code != 0)
    {
        return true;
    }

    simdjson::dom::element config;
    bool parsed = data.at(0).get(config) == simdjson::SUCCESS;
    if (!parsed)
    {
        return false;
    }

    std::string_view user;
    parsed = parsed && config["uid"].get(user) == simdjson::SUCCESS;
    
    std::string_view level;
    parsed = parsed && config["acctLv"].get(level) == simdjson::SUCCESS;
    
    std::string_view position;
    parsed = parsed && config["posMode"].get(position) == simdjson::SUCCESS;

    std::string_view permissions;
    parsed = parsed && config["perm"].get(permissions) == simdjson::SUCCESS;

    if (!parsed || !to_number(user, uid) || !to_number(level, mode))
    {
        return false;
    }

    hedge = position == "long_short_mode";
    trade = permissions.find("trade") != std::string_view::npos;

    return true;
}

bool okx_parse_balance(std::string_view json, int &code, double &available, std::string_view currency) noexcept
{
    simdjson::dom::array data;

    const bool result = parse(json, code, data);
    if (!result)
    {
        return false;
    }

    if (code != 0)
    {
        return true;
    }

    simdjson::dom::array assets;
    bool parsed = data.at(0)["details"].get(assets) == simdjson::SUCCESS;
    if (!parsed)
    {
        return false;
    }

    available = 0.0;
    for (const simdjson::dom::element asset : assets)
    {
        std::string_view name;
        parsed = asset["ccy"].get(name) == simdjson::SUCCESS;
        if (!parsed)
        {
            return false;
        }

        if (name != currency)
        {
            continue;
        }

        std::string_view text;
        parsed = asset["availEq"].get(text) == simdjson::SUCCESS;

        return parsed && to_number(text, available);
    }

    return true;
}

bool okx_parse_positions(std::string_view json, int &code, size_t &count) noexcept
{
    simdjson::dom::array data;

    const bool result = parse(json, code, data);
    if (!result)
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
        const bool parsed = position["pos"].get(text) == simdjson::SUCCESS;

        double size = 0.0;
        if (!parsed || !to_number(text, size))
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

bool okx_parse_orders(std::string_view json, int &code, size_t &count) noexcept
{
    simdjson::dom::array data;
    
    const bool result = parse(json, code, data);
    if (!result)
    {
        return false;
    }

    if (code != 0)
    {
        return true;
    }

    count = data.size();

    return true;
}

bool okx_parse_instruments(std::string_view json, int &code, std::string_view settle, std::string_view category, std::string_view contract_type,
                           milliseconds listed_before, std::span<Instrument> instruments, size_t &count, size_t &ignored) noexcept
{
    simdjson::dom::array data;

    const bool result = parse(json, code, data);
    if (!result)
    {
        return false;
    }

    if (code != 0)
    {
        return true;
    }

    count = 0;
    ignored = 0;

    for (const simdjson::dom::element instrument : data)
    {
        std::string_view id;
        bool parsed = instrument["instId"].get(id) == simdjson::SUCCESS;

        std::string_view currency;
        parsed = parsed && instrument["settleCcy"].get(currency) == simdjson::SUCCESS;

        std::string_view type;
        parsed = parsed && instrument["ctType"].get(type) == simdjson::SUCCESS;

        std::string_view state;
        parsed = parsed && instrument["state"].get(state) == simdjson::SUCCESS;
        
        std::string_view rule;
        parsed = parsed && instrument["ruleType"].get(rule) == simdjson::SUCCESS;
        
        std::string_view group;
        parsed = parsed && instrument["instCategory"].get(group) == simdjson::SUCCESS;
        
        std::string_view expiry;
        parsed = parsed && instrument["expTime"].get(expiry) == simdjson::SUCCESS;
        
        std::string_view listing;
        parsed = parsed && instrument["listTime"].get(listing) == simdjson::SUCCESS;

        if (!parsed)
        {
            return false;
        }

        const bool eligible = currency == settle && type == contract_type && state == "live" && rule == "normal" && group == category && expiry.empty();
        if (!eligible)
        {
            continue;
        }

        int64_t list_time = 0;
        if (!to_number(listing, list_time))
        {
            return false;
        }

        if (milliseconds(list_time) > listed_before)
        {
            continue;
        }

        if (count == instruments.size())
        {
            ++ignored;
            continue;
        }

        Instrument &target = instruments[count];
        if (!string::safe_strcpy(target.id, id))
        {
            ++ignored;
            continue;
        }

        ++count;
    }

    return true;
}

bool okx_parse_candles(std::string_view json, int &code, std::span<Volume> volumes, size_t &count) noexcept
{
    simdjson::dom::array data;

    const bool result = parse(json, code, data);
    if (!result)
    {
        return false;
    }

    if (code != 0)
    {
        return true;
    }

    count = 0;

    const size_t size = volumes.size();
    for (const simdjson::dom::element row : data)
    {
        if (count == size)
        {
            return false;
        }

        simdjson::dom::array fields;
        bool parsed = row.get(fields) == simdjson::SUCCESS;
        if (!parsed)
        {
            return false;
        }

        std::string_view stamp;
        parsed = parsed && fields.at(0).get(stamp) == simdjson::SUCCESS;

        std::string_view volume;
        parsed = parsed && fields.at(7).get(volume) == simdjson::SUCCESS;

        std::string_view confirm;
        parsed = parsed && fields.at(8).get(confirm) == simdjson::SUCCESS;

        Volume &target = volumes[count];
        if (!parsed || !to_number(stamp, target.timestamp) || !to_number(volume, target.volume))
        {
            return false;
        }

        target.confirmed = confirm == "1";

        ++count;
    }

    return true;
}

bool okx_parse_event(std::string_view json, std::string_view &event, std::string_view &instrument, int &code, std::string_view &message) noexcept
{
    simdjson::dom::element root;
    bool parsed = parser().parse(json.data(), json.size(), false).get(root) == simdjson::SUCCESS;
    parsed = parsed && root["event"].get(event) == simdjson::SUCCESS;

    if (!parsed)
    {
        return false;
    }

    instrument = std::string_view();
    message = std::string_view();
    code = 0;

    simdjson::dom::element argument;
    const bool has_argument = root["arg"].get(argument) == simdjson::SUCCESS;
    parsed = !has_argument || argument["instId"].get(instrument) == simdjson::SUCCESS;

    std::string_view text;
    const bool has_code = root["code"].get(text) == simdjson::SUCCESS;
    parsed = parsed && (!has_code || to_number(text, code));

    if (!parsed)
    {
        return false;
    }

    const bool has_message = root["msg"].get(message) == simdjson::SUCCESS;
    if (!has_message)
    {
        message = std::string_view();
    }

    return true;
}

bool okx_parse_candle_head(std::string_view json, std::string_view &instrument, std::string_view &body, bool &confirmed) noexcept
{
    std::string_view text = json;

    bool parsed = consume_prefix(text, R"({"arg":{"channel":"candle1m","instId":")");
    if (!parsed) [[unlikely]]
    {
        return false;
    }

    confirmed = consume_suffix(text, R"(","1"]]})");
    parsed = confirmed || consume_suffix(text, R"(","0"]]})");

    const size_t quote = text.find('"');
    parsed = parsed && quote != 0 && quote != std::string_view::npos;

    if (!parsed) [[unlikely]]
    {
        return false;
    }

    instrument = text.substr(0, quote);
    text.remove_prefix(quote);

    parsed = consume_prefix(text, R"("},"data":[[")");
    parsed = parsed && text.find(R"(],[)") == std::string_view::npos;

    body = text;

    return parsed;
}

bool okx_parse_candle_body(std::string_view body, Candle &candle) noexcept
{
    std::string_view text = body;

    bool parsed = read_field(text, candle.timestamp);
    parsed = parsed && read_field(text, candle.open);
    parsed = parsed && read_field(text, candle.high);
    parsed = parsed && read_field(text, candle.low);
    parsed = parsed && read_field(text, candle.close);
    parsed = parsed && skip_field(text);
    parsed = parsed && skip_field(text);
    parsed = parsed && read_field(text, candle.volume);

    return parsed && text.empty();
}

} // namespace nlx::parsing::detail
