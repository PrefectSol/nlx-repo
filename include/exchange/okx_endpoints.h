#ifndef NLX_EXCHANGE_OKX_ENDPOINTS_H
#define NLX_EXCHANGE_OKX_ENDPOINTS_H

#include <cstddef>

namespace nlx::exchange
{

struct OkxEndpoints
{
    const char *name;
    const char *public_url;
    const char *private_url;
    const char *business_url;
    size_t clock_milliseconds;
};

inline constexpr OkxEndpoints kOkxLive{ "live", "wss://ws.okx.com/ws/v5/public", "wss://ws.okx.com/ws/v5/private", "wss://ws.okx.com/ws/v5/business", 2000 };
inline constexpr OkxEndpoints kOkxDemo{ "demo", "wss://wspap.okx.com/ws/v5/public", "wss://wspap.okx.com/ws/v5/private", "wss://wspap.okx.com/ws/v5/business", 30000 };

[[nodiscard]] constexpr const OkxEndpoints &okx_endpoints(bool demo) noexcept
{
    return demo ? kOkxDemo : kOkxLive;
}

} // namespace nlx::exchange
#endif // !NLX_EXCHANGE_OKX_ENDPOINTS_H
