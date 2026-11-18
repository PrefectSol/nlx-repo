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

bool to_amount(std::string_view text, double &value) noexcept
{
    value = 0.0;
    return text.empty() || to_number(text, value);
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

template<typename T>
bool find_field(std::string_view text, std::string_view key, T &value) noexcept
{
    const size_t position = text.find(key);
    if (position == std::string_view::npos)
    {
        return false;
    }

    const char *const begin = text.data() + position + key.size();
    const char *const end = text.data() + text.size();

    const std::from_chars_result result = std::from_chars(begin, end, value);

    return result.ec == std::errc();
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

bool parse_push(std::string_view json, simdjson::dom::array &data) noexcept
{
    simdjson::dom::element root;
    const bool parsed = parser().parse(json.data(), json.size(), false).get(root) == simdjson::SUCCESS;

    return parsed && root["data"].get(data) == simdjson::SUCCESS;
}

bool read_text(simdjson::dom::element object, std::string_view key, std::string_view &text) noexcept
{
    return object[key].get(text) == simdjson::SUCCESS;
}

template<size_t N>
bool read_text(simdjson::dom::element object, std::string_view key, char (&text)[N]) noexcept
{
    std::string_view value;
    return read_text(object, key, value) && string::safe_strcpy(text, value);
}

bool read_amount(simdjson::dom::element object, std::string_view key, double &value) noexcept
{
    std::string_view text;
    return read_text(object, key, text) && to_amount(text, value);
}

bool read_balance(simdjson::dom::element account, std::string_view currency, Balance &balance) noexcept
{
    simdjson::dom::array assets;
    bool parsed = account["details"].get(assets) == simdjson::SUCCESS;
    if (!parsed)
    {
        return false;
    }

    balance = Balance{};
    for (const simdjson::dom::element asset : assets)
    {
        std::string_view name;
        parsed = read_text(asset, "ccy", name);
        if (!parsed)
        {
            return false;
        }

        if (name != currency)
        {
            continue;
        }

        parsed = read_amount(asset, "eq", balance.equity);
        parsed = parsed && read_amount(asset, "availBal", balance.available);
        parsed = parsed && read_amount(asset, "isoUpl", balance.isolated_pnl);

        return parsed;
    }

    return true;
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

bool okx_parse_balance(std::string_view json, int &code, std::string_view currency, Balance &balance) noexcept
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

    simdjson::dom::element account;
    const bool parsed = data.at(0).get(account) == simdjson::SUCCESS;

    return parsed && read_balance(account, currency, balance);
}

bool okx_parse_fees(std::string_view json, int &code, std::span<Fee> fees, size_t &count) noexcept
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

    simdjson::dom::array groups;
    bool parsed = data.at(0)["feeGroup"].get(groups) == simdjson::SUCCESS;
    if (!parsed)
    {
        return false;
    }

    count = 0;
    for (const simdjson::dom::element group : groups)
    {
        if (count == fees.size())
        {
            return false;
        }

        std::string_view id;
        parsed = group["groupId"].get(id) == simdjson::SUCCESS;

        std::string_view maker;
        parsed = parsed && group["maker"].get(maker) == simdjson::SUCCESS;

        std::string_view taker;
        parsed = parsed && group["taker"].get(taker) == simdjson::SUCCESS;

        Fee &target = fees[count];
        if (!parsed || !to_number(id, target.group) || !to_number(maker, target.maker) || !to_number(taker, target.taker))
        {
            return false;
        }

        ++count;
    }

    return true;
}

bool okx_parse_tiers(std::string_view json, int &code, std::span<Tier> tiers, size_t &count) noexcept
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
    for (const simdjson::dom::element tier : data)
    {
        if (count == tiers.size())
        {
            return false;
        }

        std::string_view minimum;
        bool parsed = tier["minSz"].get(minimum) == simdjson::SUCCESS;

        std::string_view maximum;
        parsed = parsed && tier["maxSz"].get(maximum) == simdjson::SUCCESS;

        std::string_view leverage;
        parsed = parsed && tier["maxLever"].get(leverage) == simdjson::SUCCESS;

        Tier &target = tiers[count];
        if (!parsed || !to_number(minimum, target.min_size) || !to_number(maximum, target.max_size) || !to_number(leverage, target.max_leverage))
        {
            return false;
        }

        ++count;
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

        std::string_view family;
        parsed = parsed && instrument["instFamily"].get(family) == simdjson::SUCCESS;

        std::string_view value;
        parsed = parsed && instrument["ctVal"].get(value) == simdjson::SUCCESS;

        std::string_view multiplier;
        parsed = parsed && instrument["ctMult"].get(multiplier) == simdjson::SUCCESS;

        std::string_view lot;
        parsed = parsed && instrument["lotSz"].get(lot) == simdjson::SUCCESS;

        std::string_view minimum;
        parsed = parsed && instrument["minSz"].get(minimum) == simdjson::SUCCESS;

        std::string_view tick;
        parsed = parsed && instrument["tickSz"].get(tick) == simdjson::SUCCESS;

        std::string_view maximum;
        parsed = parsed && instrument["maxLmtSz"].get(maximum) == simdjson::SUCCESS;

        std::string_view leverage;
        parsed = parsed && instrument["lever"].get(leverage) == simdjson::SUCCESS;

        std::string_view fee;
        parsed = parsed && instrument["groupId"].get(fee) == simdjson::SUCCESS;

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

        bool converted = string::safe_strcpy(target.id, id) && string::safe_strcpy(target.family, family);
        converted = converted && to_number(value, target.contract_value) && to_number(multiplier, target.contract_multiplier);
        converted = converted && to_number(lot, target.lot_size) && to_number(minimum, target.min_size) && to_number(tick, target.tick_size);
        converted = converted && to_number(maximum, target.max_limit_size) && to_number(leverage, target.max_leverage);
        converted = converted && to_number(fee, target.fee_group);

        if (!converted)
        {
            ++ignored;
            continue;
        }

        target.accepted = false;

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

    const bool has_instrument = has_argument && argument["instId"].get(instrument) == simdjson::SUCCESS;
    if (!has_instrument)
    {
        instrument = std::string_view();
    }

    std::string_view text;
    const bool has_code = root["code"].get(text) == simdjson::SUCCESS;
    parsed = !has_code || to_number(text, code);

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

bool okx_parse_market_head(std::string_view json, std::string_view &channel, std::string_view &instrument, std::string_view &body) noexcept
{
    std::string_view text = json;

    bool parsed = consume_prefix(text, R"({"arg":{"channel":")");
    if (!parsed) [[unlikely]]
    {
        return false;
    }

    const size_t name = text.find('"');
    if (name == std::string_view::npos) [[unlikely]]
    {
        return false;
    }

    channel = text.substr(0, name);
    text.remove_prefix(name);

    parsed = consume_prefix(text, R"(","instId":")");

    const size_t quote = text.find('"');
    parsed = parsed && quote != 0 && quote != std::string_view::npos;

    if (!parsed) [[unlikely]]
    {
        return false;
    }

    instrument = text.substr(0, quote);
    text.remove_prefix(quote);

    parsed = consume_prefix(text, R"("},)");
    body = text;

    return parsed;
}

bool okx_parse_trades(std::string_view body, std::string_view &trades) noexcept
{
    std::string_view text = body;

    bool parsed = consume_prefix(text, R"("data":[)");
    parsed = parsed && consume_suffix(text, R"(]})");

    trades = text;

    return parsed;
}

bool okx_parse_trade(std::string_view &trades, Trade &trade) noexcept
{
    const size_t end = trades.find('}');
    if (end == std::string_view::npos) [[unlikely]]
    {
        return false;
    }

    const std::string_view object = trades.substr(0, end);

    bool parsed = find_field(object, R"("px":")", trade.price);
    parsed = parsed && find_field(object, R"("sz":")", trade.size);
    parsed = parsed && find_field(object, R"("ts":")", trade.timestamp);

    trades.remove_prefix(end + 1);
    [[maybe_unused]] const bool separated = consume_prefix(trades, ",");

    return parsed;
}

bool okx_parse_books(std::string_view body, bool &snapshot, std::string_view &asks, std::string_view &bids,
                     int64_t &timestamp, int64_t &sequence, int64_t &previous) noexcept
{
    constexpr std::string_view bids_key = R"(],"bids":[)";
    constexpr std::string_view tail_key = R"(],"ts":")";

    std::string_view text = body;

    snapshot = consume_prefix(text, R"("action":"snapshot","data":[{"asks":[)");

    const bool update = !snapshot && consume_prefix(text, R"("action":"update","data":[{"asks":[)");
    if (!snapshot && !update) [[unlikely]]
    {
        return false;
    }

    const size_t middle = text.find(bids_key);
    if (middle == std::string_view::npos) [[unlikely]]
    {
        return false;
    }

    asks = text.substr(0, middle);
    text.remove_prefix(middle + bids_key.size());

    const size_t tail = text.find(tail_key);
    if (tail == std::string_view::npos) [[unlikely]]
    {
        return false;
    }

    bids = text.substr(0, tail);
    text.remove_prefix(tail + 2);

    bool parsed = find_field(text, R"("ts":")", timestamp);
    parsed = parsed && find_field(text, R"("seqId":)", sequence);
    parsed = parsed && find_field(text, R"("prevSeqId":)", previous);

    return parsed && text.ends_with(R"(}]})");
}

bool okx_parse_level(std::string_view &levels, Level &level) noexcept
{
    bool parsed = consume_prefix(levels, R"([")");
    parsed = parsed && read_field(levels, level.price);
    parsed = parsed && read_field(levels, level.size);

    const size_t end = levels.find(']');
    if (!parsed || end == std::string_view::npos) [[unlikely]]
    {
        return false;
    }

    levels.remove_prefix(end + 1);
    [[maybe_unused]] const bool separated = consume_prefix(levels, ",");

    return true;
}

bool okx_parse_channel(std::string_view json, std::string_view &channel) noexcept
{
    std::string_view text = json;

    const bool parsed = consume_prefix(text, R"({"arg":{"channel":")");
    const size_t quote = text.find('"');

    if (!parsed || quote == std::string_view::npos)
    {
        return false;
    }

    channel = text.substr(0, quote);

    return true;
}

bool okx_parse_account_push(std::string_view json, std::string_view currency, Balance &balance, int64_t &timestamp) noexcept
{
    simdjson::dom::array data;
    if (!parse_push(json, data))
    {
        return false;
    }

    simdjson::dom::element account;
    bool parsed = data.at(0).get(account) == simdjson::SUCCESS;

    std::string_view updated;
    parsed = parsed && read_text(account, "uTime", updated);

    return parsed && to_number(updated, timestamp) && read_balance(account, currency, balance);
}

bool okx_parse_positions_push(std::string_view json, std::span<Position> positions, size_t &count) noexcept
{
    simdjson::dom::array data;
    if (!parse_push(json, data))
    {
        return false;
    }

    count = 0;
    for (const simdjson::dom::element position : data)
    {
        if (count == positions.size())
        {
            return false;
        }

        Position &target = positions[count];

        bool parsed = read_text(position, "instId", target.instrument);
        parsed = parsed && read_text(position, "posSide", target.side);
        parsed = parsed && read_amount(position, "pos", target.size);
        parsed = parsed && read_amount(position, "avgPx", target.price);
        parsed = parsed && read_amount(position, "liqPx", target.liquidation);
        parsed = parsed && read_amount(position, "margin", target.margin);
        parsed = parsed && read_amount(position, "upl", target.pnl);

        if (!parsed)
        {
            return false;
        }

        ++count;
    }

    return true;
}

bool okx_parse_orders_push(std::string_view json, std::span<Order> orders, size_t &count) noexcept
{
    simdjson::dom::array data;
    if (!parse_push(json, data))
    {
        return false;
    }

    count = 0;
    for (const simdjson::dom::element order : data)
    {
        if (count == orders.size())
        {
            return false;
        }

        Order &target = orders[count];

        bool parsed = read_text(order, "instId", target.instrument);
        parsed = parsed && read_text(order, "ordId", target.id);
        parsed = parsed && read_text(order, "clOrdId", target.client);
        parsed = parsed && read_text(order, "side", target.side);
        parsed = parsed && read_text(order, "posSide", target.position);
        parsed = parsed && read_text(order, "state", target.state);
        parsed = parsed && read_amount(order, "sz", target.size);
        parsed = parsed && read_amount(order, "accFillSz", target.filled);
        parsed = parsed && read_amount(order, "avgPx", target.price);

        if (!parsed)
        {
            return false;
        }

        ++count;
    }

    return true;
}

} // namespace nlx::parsing::detail
