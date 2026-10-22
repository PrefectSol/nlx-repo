#ifndef NLX_PARSING_JSON_READER_H
#define NLX_PARSING_JSON_READER_H

#include "core/time.h"
#include "exchange/okx_types.h"

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
[[nodiscard]] bool okx_parse_balance(std::string_view json, int &code, double &available, std::string_view currency) noexcept;
[[nodiscard]] bool okx_parse_positions(std::string_view json, int &code, size_t &count) noexcept;
[[nodiscard]] bool okx_parse_orders(std::string_view json, int &code, size_t &count) noexcept;
[[nodiscard]] bool okx_parse_linear_swaps(std::string_view json, int &code, std::string_view settle, std::string_view category, milliseconds listed_before,
                                          std::span<exchange::OkxInstrument> instruments, size_t &count, size_t &ignored) noexcept;
[[nodiscard]] bool okx_parse_candles(std::string_view json, int &code, std::span<exchange::OkxCandle> candles, size_t &count) noexcept;

} // namespace nlx::parsing::detail
} // namespace nlx::parsing
#endif // !NLX_PARSING_JSON_READER_H
