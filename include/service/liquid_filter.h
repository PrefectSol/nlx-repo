#ifndef NLX_SERVICE_LIQUID_FILTER_H
#define NLX_SERVICE_LIQUID_FILTER_H

#include "core/config.h"

#include <cstddef>
#include <span>

namespace nlx::service
{

class LiquidFilter
{
public:
    explicit LiquidFilter(const StrategyConfig &config) noexcept;

    LiquidFilter(const LiquidFilter &) = delete;
    LiquidFilter &operator=(const LiquidFilter &) = delete;
    LiquidFilter(LiquidFilter &&) = delete;
    LiquidFilter &operator=(LiquidFilter &&) = delete;

    void apply(std::span<const double> volumes, std::span<bool> passed) const noexcept;

private:
    size_t m_days;

    [[nodiscard]] bool accept(std::span<const double> volumes) const noexcept;
};

} // namespace nlx::service
#endif // !NLX_SERVICE_LIQUID_FILTER_H
