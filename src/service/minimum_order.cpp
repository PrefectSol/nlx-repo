#include "service/minimum_order.h"

#include <algorithm>

namespace nlx::detail
{

bool fit_minimum_order(const Instrument &instrument, std::span<const Tier> tiers, double budget, double price, double fee,
                       double leverage, service::MinimumOrder &order, std::string_view &rejection) noexcept
{
    order = service::MinimumOrder{};

    if (budget <= 0.0)
    {
        rejection = "budget";
        return false;
    }

    if (price <= 0.0)
    {
        rejection = "price";
        return false;
    }

    const double contract = instrument.contract_value * instrument.contract_multiplier;

    const bool valid = contract > 0.0 && instrument.min_size > 0.0 && instrument.max_leverage > 0.0;
    if (!valid)
    {
        rejection = "instrument";
        return false;
    }

    double lever = std::min(leverage, instrument.max_leverage);

    if (!tiers.empty())
    {
        const Tier *found = nullptr;
        for (const Tier &tier : tiers)
        {
            const bool inside = instrument.min_size > tier.min_size && instrument.min_size <= tier.max_size;
            if (inside)
            {
                found = &tier;
                break;
            }
        }

        if (found == nullptr)
        {
            rejection = "tier";
            return false;
        }

        lever = std::min(lever, found->max_leverage);
    }

    if (lever < 1.0)
    {
        rejection = "leverage";
        return false;
    }

    order.leverage = lever;
    order.size = instrument.min_size;
    order.notional = instrument.min_size * contract * price;
    order.margin = order.notional / lever;
    order.fees = 2.0 * fee * order.notional;

    if (order.margin + order.fees > budget)
    {
        rejection = "minimum";
        return false;
    }

    return true;
}

} // namespace nlx::detail
