#ifndef NLX_CORE_MARKET_H
#define NLX_CORE_MARKET_H

#include <cstddef>
#include <cstdint>

namespace nlx
{

struct Instrument
{
    static constexpr size_t kIdCapacity = 32;

    char id[kIdCapacity];
    bool accepted;
};

struct Candle
{
    int64_t timestamp;
    double open;
    double high;
    double low;
    double close;
    double volume;
};

struct Volume
{
    int64_t timestamp;
    double volume;
    bool confirmed;
};

} // namespace nlx
#endif // !NLX_CORE_MARKET_H
