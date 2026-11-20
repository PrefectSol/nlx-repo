#ifndef NLX_CORE_METRICS_H
#define NLX_CORE_METRICS_H

#include "core/time.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace nlx
{

class LatencyHistogram
{
public:
    LatencyHistogram() noexcept;

    void record(nanoseconds value) noexcept;
    void reset() noexcept;

    [[nodiscard]] size_t count() const noexcept;
    [[nodiscard]] double percentile(double quantile) const noexcept;
    [[nodiscard]] double mean() const noexcept;
    [[nodiscard]] double max() const noexcept;

private:
    static constexpr size_t kSubBuckets = 8;
    static constexpr size_t kBuckets = 64 * kSubBuckets;

    std::array<uint64_t, kBuckets> m_buckets;

    uint64_t m_count;
    uint64_t m_sum;
    uint64_t m_max;
};

class LoopMeter
{
public:
    LoopMeter() noexcept;

    void wake(nanoseconds now) noexcept;
    void sleep(nanoseconds now) noexcept;
    void reset(nanoseconds now) noexcept;

    [[nodiscard]] double utilization(nanoseconds now) const noexcept;
    [[nodiscard]] size_t wakeups() const noexcept;

private:
    nanoseconds m_window;
    nanoseconds m_busy;
    nanoseconds m_woken;
    size_t m_wakeups;
    bool m_awake;
};

} // namespace nlx
#endif // !NLX_CORE_METRICS_H
