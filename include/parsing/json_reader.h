#ifndef NLX_PARSING_JSON_READER_H
#define NLX_PARSING_JSON_READER_H

#include "core/market.h"
#include "core/time.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace nlx::parsing
{

inline constexpr size_t json_padding_size = 64;

namespace detail
{

[[nodiscard]] bool okx_parse_code(std::string_view json, int &code) noexcept;
[[nodiscard]] bool okx_parse_time(std::string_view json, int &code, milliseconds &time) noexcept;
[[nodiscard]] bool okx_parse_account_config(std::string_view json, int &code, uint64_t &uid, int &mode, bool &hedge, bool &trade) noexcept;
[[nodiscard]] bool okx_parse_balance(std::string_view json, int &code, std::string_view currency, Balance &balance) noexcept;
[[nodiscard]] bool okx_parse_fees(std::string_view json, int &code, std::span<Fee> fees, size_t &count) noexcept;
[[nodiscard]] bool okx_parse_tiers(std::string_view json, int &code, std::span<Tier> tiers, size_t &count) noexcept;
[[nodiscard]] bool okx_parse_positions(std::string_view json, int &code, size_t &count) noexcept;
[[nodiscard]] bool okx_parse_orders(std::string_view json, int &code, size_t &count) noexcept;
[[nodiscard]] bool okx_parse_instruments(std::string_view json, int &code, std::string_view settle, std::string_view category, std::string_view contract_type,
                                         milliseconds listed_before, std::span<Instrument> instruments, size_t &count, size_t &ignored) noexcept;
[[nodiscard]] bool okx_parse_candles(std::string_view json, int &code, std::span<Volume> volumes, size_t &count) noexcept;
[[nodiscard]] bool okx_parse_event(std::string_view json, std::string_view &event, std::string_view &instrument, int &code, std::string_view &message) noexcept;
[[nodiscard]] bool okx_parse_candle_head(std::string_view json, std::string_view &instrument, std::string_view &body, bool &confirmed) noexcept;
[[nodiscard]] bool okx_parse_candle_body(std::string_view body, Candle &candle) noexcept;

[[nodiscard]] bool okx_parse_market_head(std::string_view json, std::string_view &channel, std::string_view &instrument, std::string_view &body) noexcept;
[[nodiscard]] bool okx_parse_trades(std::string_view body, std::string_view &trades) noexcept;
[[nodiscard]] bool okx_parse_trade(std::string_view &trades, Trade &trade) noexcept;
[[nodiscard]] bool okx_parse_books(std::string_view body, bool &snapshot, std::string_view &asks, std::string_view &bids,
                                   int64_t &timestamp, int64_t &sequence, int64_t &previous) noexcept;
[[nodiscard]] bool okx_parse_level(std::string_view &levels, Level &level) noexcept;

[[nodiscard]] bool okx_parse_channel(std::string_view json, std::string_view &channel) noexcept;
[[nodiscard]] bool okx_parse_account_push(std::string_view json, std::string_view currency, Balance &balance, int64_t &timestamp) noexcept;
[[nodiscard]] bool okx_parse_positions_push(std::string_view json, std::span<Position> positions, size_t &count) noexcept;
[[nodiscard]] bool okx_parse_orders_push(std::string_view json, std::span<Order> orders, size_t &count) noexcept;

} // namespace nlx::parsing::detail
} // namespace nlx::parsing
#endif // !NLX_PARSING_JSON_READER_H
