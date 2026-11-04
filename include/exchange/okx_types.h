#ifndef NLX_EXCHANGE_OKX_TYPES_H
#define NLX_EXCHANGE_OKX_TYPES_H

#include <cstddef>
#include <cstdint>

namespace nlx::exchange
{

struct OkxInstrument
{
    static constexpr size_t kIdCapacity = 32;

    char id[kIdCapacity];
    bool accepted;
};

struct OkxVolume
{
    int64_t timestamp;
    double volume_usdt;
    bool confirmed;
};

} // namespace nlx::exchange
#endif // !NLX_EXCHANGE_OKX_TYPES_H
