#ifndef NLX_SERVICE_MINIMUM_ORDER_H
#define NLX_SERVICE_MINIMUM_ORDER_H

#include "core/market.h"

#include <span>
#include <string_view>

namespace nlx::service
{

struct MinimumOrder
{
    double leverage;
    double size;
    double notional;
    double margin;
    double fees;
};

} // namespace nlx::service

namespace nlx::detail
{

[[nodiscard]] bool fit_minimum_order(const Instrument &instrument, std::span<const Tier> tiers, double budget, double price, double fee,
                                     double leverage, service::MinimumOrder &order, std::string_view &rejection) noexcept;

} // namespace nlx::detail
#endif // !NLX_SERVICE_MINIMUM_ORDER_H
