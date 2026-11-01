#ifndef NLX_EXCHANGE_OKX_TYPES_H
#define NLX_EXCHANGE_OKX_TYPES_H

#include "core/time.h"

#include <cstddef>

namespace nlx::exchange
{

struct OkxInstrument
{
    static constexpr size_t kIdCapacity = 32;

    char id[kIdCapacity];
    double contract_value;
    milliseconds list_time;
    bool is_liquid;
};

struct OkxCandle
{
    milliseconds time;
    double quote_volume;
    bool confirmed;
};

struct alignas(64) OkxBar
{
    milliseconds time;
    double open;
    double high;
    double low;
    double close;
    double volume;
    double currency_volume;
    double quote_volume;
};

} // namespace nlx::exchange
#endif // !NLX_EXCHANGE_OKX_TYPES_H
