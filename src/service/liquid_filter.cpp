#include "service/liquid_filter.h"

namespace nlx::service
{

LiquidFilter::LiquidFilter(const StrategyConfig &config) noexcept :
    m_days(static_cast<size_t>(config.liquidity_window_days))
{

}

void LiquidFilter::apply(std::span<const double> volumes, std::span<bool> passed) const noexcept
{
    const size_t size = passed.size();
    for (size_t index = 0; index < size; ++index)
    {
        passed[index] = accept(volumes.subspan(index * m_days, m_days));
    }
}

bool LiquidFilter::accept(std::span<const double> volumes) const noexcept
{
    return true;
}

} // namespace nlx::service
