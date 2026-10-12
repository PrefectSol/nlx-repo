#ifndef NLX_CORE_SPSC_QUEUE_H
#define NLX_CORE_SPSC_QUEUE_H

#define NO_USE_STDC_HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <algorithm>

#ifndef NO_USE_STDC_HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE
#include <new>
#endif // !NO_USE_STDC_HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE

namespace nlx 
{

template<typename T, size_t Capacity>
class SpscQueue
{
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::is_default_constructible_v<T>);
    static_assert(std::is_nothrow_default_constructible_v<T>);
    static_assert(std::is_nothrow_copy_assignable_v<T>);
    static_assert(Capacity >= 1);
    static_assert((Capacity & (Capacity - 1)) == 0);

public:
    SpscQueue() = default;
    ~SpscQueue() = default;

    SpscQueue(const SpscQueue &) = delete;
    SpscQueue &operator=(const SpscQueue &) = delete;
    SpscQueue(SpscQueue &&) = delete;
    SpscQueue &operator=(SpscQueue &&) = delete;
    
    [[nodiscard]] bool push(const T &item) noexcept;
    [[nodiscard]] bool pop(T &item) noexcept;

    [[nodiscard]] size_t push(const T *items, size_t size) noexcept;
    [[nodiscard]] size_t pop(T *items, size_t size) noexcept;

    [[nodiscard]] T *claim() noexcept;
    [[nodiscard]] bool commit() noexcept;
    
    [[nodiscard]] const T *front() noexcept;
    [[nodiscard]] bool consume() noexcept;

    [[nodiscard]] bool empty_approx() const noexcept;
    [[nodiscard]] size_t size_approx() const noexcept;

    [[nodiscard]] static constexpr size_t capacity() noexcept;

private:
    static constexpr size_t kMask = Capacity - 1;

#if !defined(NO_USE_STDC_HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE) && defined(__cpp_lib_hardware_interference_size)
    static constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
    static constexpr size_t kCacheLineSize = 64;
#endif
    
    struct alignas(kCacheLineSize) State
    {
        std::atomic<size_t> index{0};
        size_t shadow_index{0};
        bool pending{false};
    };

    State m_producer;
    State m_consumer;

    alignas(kCacheLineSize) T m_buffer[Capacity];
};

} // namespace nlx

namespace nlx 
{

template<typename T, size_t Capacity>
bool SpscQueue<T, Capacity>::push(const T &item) noexcept
{
    if (m_producer.pending) [[unlikely]]
    {
        return false;
    }

    const size_t position = m_producer.index.load(std::memory_order_relaxed);
    if (position - m_producer.shadow_index == Capacity) [[unlikely]]
    {
        m_producer.shadow_index = m_consumer.index.load(std::memory_order_acquire);
        if (position - m_producer.shadow_index == Capacity) [[unlikely]]
        {
            return false;
        }
    }

    m_buffer[position & kMask] = item;
    m_producer.index.store(position + 1, std::memory_order_release);
    
    return true;
}

template<typename T, size_t Capacity>
bool SpscQueue<T, Capacity>::pop(T &item) noexcept
{
    if (m_consumer.pending) [[unlikely]]
    {
        return false;
    }

    const size_t position = m_consumer.index.load(std::memory_order_relaxed);
    if (position == m_consumer.shadow_index) [[unlikely]]
    {
        m_consumer.shadow_index = m_producer.index.load(std::memory_order_acquire);
        if (position == m_consumer.shadow_index) [[unlikely]]
        {
            return false;
        }
    }

    item = m_buffer[position & kMask];
    m_consumer.index.store(position + 1, std::memory_order_release);

    return true;
}

template<typename T, size_t Capacity>
size_t SpscQueue<T, Capacity>::push(const T *items, size_t size) noexcept
{
    if (items == nullptr || size == 0) [[unlikely]]
    {
        return 0;
    }

    if (m_producer.pending) [[unlikely]]
    {
        return 0;
    }

    const size_t position = m_producer.index.load(std::memory_order_relaxed);
    
    size_t available = Capacity - (position - m_producer.shadow_index);
    if (available < size) [[unlikely]]
    {
        m_producer.shadow_index = m_consumer.index.load(std::memory_order_acquire);
        available = Capacity - (position - m_producer.shadow_index);
    }

    const size_t actual = std::min(size, available);
    if (actual == 0) [[unlikely]]
    {
        return 0;
    }

    const size_t index = position & kMask;
    const size_t chunk1 = std::min(actual, Capacity - index);
    std::copy_n(items, chunk1, m_buffer + index);
    
    const size_t chunk2 = actual - chunk1;
    if (chunk2 != 0)
    {
        std::copy_n(items + chunk1, chunk2, m_buffer);
    }

    m_producer.index.store(position + actual, std::memory_order_release);

    return actual;
}

template<typename T, size_t Capacity>
size_t SpscQueue<T, Capacity>::pop(T *items, size_t size) noexcept
{
    if (items == nullptr || size == 0) [[unlikely]]
    {
        return 0;
    }

    if (m_consumer.pending) [[unlikely]]
    {
        return 0;
    }

    const size_t position = m_consumer.index.load(std::memory_order_relaxed);
    
    size_t available = m_consumer.shadow_index - position;
    if (available < size) [[unlikely]]
    {
        m_consumer.shadow_index = m_producer.index.load(std::memory_order_acquire);
        available = m_consumer.shadow_index - position;
    }

    const size_t actual = std::min(size, available);
    if (actual == 0) [[unlikely]]
    {
        return 0;
    }

    const size_t index = position & kMask;
    const size_t chunk1 = std::min(actual, Capacity - index);
    std::copy_n(m_buffer + index, chunk1, items);

    const size_t chunk2 = actual - chunk1;
    if (chunk2 != 0)
    {
        std::copy_n(m_buffer, chunk2, items + chunk1);
    }

    m_consumer.index.store(position + actual, std::memory_order_release);

    return actual;
}

template<typename T, size_t Capacity>
T *SpscQueue<T, Capacity>::claim() noexcept
{
    if (m_producer.pending) [[unlikely]]
    {
        return nullptr;
    }

    const size_t position = m_producer.index.load(std::memory_order_relaxed);
    if (position - m_producer.shadow_index == Capacity) [[unlikely]]
    {
        m_producer.shadow_index = m_consumer.index.load(std::memory_order_acquire);
        if (position - m_producer.shadow_index == Capacity) [[unlikely]]
        {
            return nullptr;
        }
    }

    m_producer.pending = true;

    return &m_buffer[position & kMask];
}

template<typename T, size_t Capacity>
bool SpscQueue<T, Capacity>::commit() noexcept
{
    if (!m_producer.pending) [[unlikely]]
    {
        return false;
    }

    const size_t position = m_producer.index.load(std::memory_order_relaxed);
    m_producer.index.store(position + 1, std::memory_order_release);
    m_producer.pending = false;

    return true;
}

template<typename T, size_t Capacity>
const T *SpscQueue<T, Capacity>::front() noexcept
{
    if (m_consumer.pending) [[unlikely]]
    {
        return nullptr;
    }

    const size_t position = m_consumer.index.load(std::memory_order_relaxed);
    if (position == m_consumer.shadow_index) [[unlikely]]
    {
        m_consumer.shadow_index = m_producer.index.load(std::memory_order_acquire);
        if (position == m_consumer.shadow_index) [[unlikely]]
        {
            return nullptr;
        }
    }

    m_consumer.pending = true;

    return &m_buffer[position & kMask];
}

template<typename T, size_t Capacity>
bool SpscQueue<T, Capacity>::consume() noexcept
{
    if (!m_consumer.pending) [[unlikely]]
    {
        return false;
    }

    const size_t position = m_consumer.index.load(std::memory_order_relaxed);
    m_consumer.index.store(position + 1, std::memory_order_release);
    m_consumer.pending = false;

    return true;
}

template<typename T, size_t Capacity>
bool SpscQueue<T, Capacity>::empty_approx() const noexcept
{
    const size_t consumer = m_consumer.index.load(std::memory_order_relaxed);
    const size_t producer = m_producer.index.load(std::memory_order_relaxed);

    return producer == consumer;
}

template<typename T, size_t Capacity>
size_t SpscQueue<T, Capacity>::size_approx() const noexcept
{
    const size_t consumer = m_consumer.index.load(std::memory_order_relaxed);
    const size_t producer = m_producer.index.load(std::memory_order_relaxed);

    return std::min(producer - consumer, Capacity);
}

template<typename T, size_t Capacity>
constexpr size_t SpscQueue<T, Capacity>::capacity() noexcept
{
    return Capacity;
}

} // namespace nlx
#endif // !NLX_CORE_SPSC_QUEUE_H
