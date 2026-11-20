#include "core/metrics.h"

#include <algorithm>
#include <bit>

namespace nlx
{

namespace
{

constexpr size_t bucket(uint64_t value, size_t sub) noexcept
{
    if (value < sub)
    {
        return static_cast<size_t>(value);
    }

    const size_t octave = static_cast<size_t>(std::bit_width(value)) - 1;
    const size_t shift = octave - static_cast<size_t>(std::countr_zero(sub));
    const size_t fraction = static_cast<size_t>(value >> shift) - sub;

    return octave * sub + fraction;
}

constexpr uint64_t upper(size_t index, size_t sub) noexcept
{
    if (index < sub)
    {
        return index + 1;
    }

    const size_t octave = index / sub;
    const size_t fraction = index % sub;
    const size_t shift = octave - static_cast<size_t>(std::countr_zero(sub));

    return static_cast<uint64_t>(sub + fraction + 1) << shift;
}

} // namespace

LatencyHistogram::LatencyHistogram() noexcept :
    m_buckets(),
    m_count(0),
    m_max(0)
{

}

void LatencyHistogram::record(nanoseconds value) noexcept
{
    const uint64_t ticks = value.count() > 0 ? static_cast<uint64_t>(value.count()) : 0;
    const size_t index = std::min(bucket(ticks, kSubBuckets), kBuckets - 1);

    ++m_buckets[index];
    ++m_count;
    m_max = std::max(m_max, ticks);
}

void LatencyHistogram::reset() noexcept
{
    m_buckets.fill(0);
    m_count = 0;
    m_max = 0;
}

size_t LatencyHistogram::count() const noexcept
{
    return m_count;
}

double LatencyHistogram::percentile(double quantile) const noexcept
{
    if (m_count == 0)
    {
        return 0.0;
    }

    const uint64_t target = std::max<uint64_t>(1, static_cast<uint64_t>(quantile * static_cast<double>(m_count) + 0.5));

    uint64_t seen = 0;
    for (size_t index = 0; index < kBuckets; ++index)
    {
        seen += m_buckets[index];
        if (seen >= target)
        {
            return static_cast<double>(std::min(upper(index, kSubBuckets), m_max)) / 1000.0;
        }
    }

    return static_cast<double>(m_max) / 1000.0;
}

double LatencyHistogram::max() const noexcept
{
    return static_cast<double>(m_max) / 1000.0;
}

LoopMeter::LoopMeter() noexcept :
    m_window(0),
    m_busy(0),
    m_woken(0),
    m_wakeups(0),
    m_awake(false)
{

}

void LoopMeter::wake(nanoseconds now) noexcept
{
    m_woken = now;
    m_awake = true;
    ++m_wakeups;
}

void LoopMeter::sleep(nanoseconds now) noexcept
{
    if (m_awake)
    {
        m_busy += now - m_woken;
        m_awake = false;
    }
}

void LoopMeter::reset(nanoseconds now) noexcept
{
    m_window = now;
    m_busy = nanoseconds::zero();
    m_woken = now;
    m_wakeups = 0;
}

double LoopMeter::utilization(nanoseconds now) const noexcept
{
    const nanoseconds busy = m_awake ? m_busy + (now - m_woken) : m_busy;
    const nanoseconds window = now - m_window;

    return window.count() > 0 ? 100.0 * static_cast<double>(busy.count()) / static_cast<double>(window.count()) : 0.0;
}

size_t LoopMeter::wakeups() const noexcept
{
    return m_wakeups;
}

} // namespace nlx
