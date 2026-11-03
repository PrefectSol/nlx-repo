#ifndef NLX_CORE_CONCURRENT_RING_BUFFER_H
#define NLX_CORE_CONCURRENT_RING_BUFFER_H

#include "core/fatal.h"
#include "core/file_descriptor.h"
#include "core/system_error.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <numeric>
#include <span>
#include <stdexcept>
#include <type_traits>

#include <sys/mman.h>
#include <unistd.h>

namespace nlx
{

template<typename T, size_t Capacity>
class ConcurrentRingBuffer
{
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::is_nothrow_copy_assignable_v<T>);
    static_assert(Capacity >= 1);

public:
    ConcurrentRingBuffer();
    ~ConcurrentRingBuffer() noexcept;

    ConcurrentRingBuffer(const ConcurrentRingBuffer &) = delete;
    ConcurrentRingBuffer &operator=(const ConcurrentRingBuffer &) = delete;
    ConcurrentRingBuffer(ConcurrentRingBuffer &&) = delete;
    ConcurrentRingBuffer &operator=(ConcurrentRingBuffer &&) = delete;

    void clear() noexcept;

    void push(const T &item) noexcept;

    [[nodiscard]] T &claim() noexcept;
    void commit() noexcept;

    [[nodiscard]] T &back() noexcept;

    [[nodiscard]] std::span<const T, Capacity> history() const noexcept;

    [[nodiscard]] bool full() const noexcept;
    [[nodiscard]] size_t size() const noexcept;

    [[nodiscard]] static constexpr size_t capacity() noexcept;

private:
    T *m_data;
    size_t m_mask;
    std::atomic<size_t> m_head;
};

} // namespace nlx

namespace nlx
{

template<typename T, size_t Capacity>
ConcurrentRingBuffer<T, Capacity>::ConcurrentRingBuffer() :
    m_data(nullptr),
    m_mask(0),
    m_head(0)
{
    const size_t page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    if (!std::has_single_bit(page)) [[unlikely]]
    {
        throw std::runtime_error("ring buffer invalid page size");
    }

    const size_t slots = std::max(std::bit_ceil(Capacity + 1), page / std::gcd(page, sizeof(T)));
    const size_t bytes = slots * sizeof(T);

    const FileDescriptor memory = FileDescriptor::make_memfd("nlx-concurrent_ring_buffer");

    const int result = ::ftruncate(memory.get(), static_cast<off_t>(bytes));
    if (result != 0) [[unlikely]]
    {
        throw SystemError("ftruncate failed");
    }

    void *const region = ::mmap(nullptr, bytes * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) [[unlikely]]
    {
        throw SystemError("mmap reserve failed");
    }

    char *const base = static_cast<char *>(region);
    const int flags = MAP_SHARED | MAP_FIXED | MAP_POPULATE;
    const int prot = PROT_READ | PROT_WRITE;

    bool mapped = ::mmap(base, bytes, prot, flags, memory.get(), 0) != MAP_FAILED;
    mapped = mapped && ::mmap(base + bytes, bytes, prot, flags, memory.get(), 0) != MAP_FAILED;

    if (!mapped) [[unlikely]]
    {
        const SystemError error("mmap mirror failed");
        ::munmap(region, bytes * 2);

        throw error;
    }

    m_data = static_cast<T *>(region);
    m_mask = slots - 1;
}

template<typename T, size_t Capacity>
ConcurrentRingBuffer<T, Capacity>::~ConcurrentRingBuffer() noexcept
{
    const size_t bytes = (m_mask + 1) * sizeof(T);

    const int result = ::munmap(m_data, bytes * 2);
    if (result != 0) [[unlikely]]
    {
        NLX_FATAL("nlx::ConcurrentRingBuffer::~ConcurrentRingBuffer() munmap failed");
    }
}

template<typename T, size_t Capacity>
void ConcurrentRingBuffer<T, Capacity>::clear() noexcept
{
    m_head.store(0, std::memory_order_release);
}

template<typename T, size_t Capacity>
void ConcurrentRingBuffer<T, Capacity>::push(const T &item) noexcept
{
    const size_t position = m_head.load(std::memory_order_relaxed);

    m_data[position & m_mask] = item;
    m_head.store(position + 1, std::memory_order_release);
}

template<typename T, size_t Capacity>
T &ConcurrentRingBuffer<T, Capacity>::claim() noexcept
{
    const size_t position = m_head.load(std::memory_order_relaxed);
    return m_data[position & m_mask];
}

template<typename T, size_t Capacity>
void ConcurrentRingBuffer<T, Capacity>::commit() noexcept
{
    const size_t position = m_head.load(std::memory_order_relaxed);
    m_head.store(position + 1, std::memory_order_release);
}

template<typename T, size_t Capacity>
T &ConcurrentRingBuffer<T, Capacity>::back() noexcept
{
    const size_t position = m_head.load(std::memory_order_relaxed);
    return m_data[(position - 1) & m_mask];
}

template<typename T, size_t Capacity>
std::span<const T, Capacity> ConcurrentRingBuffer<T, Capacity>::history() const noexcept
{
    const size_t position = m_head.load(std::memory_order_acquire);
    return std::span<const T, Capacity>(m_data + ((position - Capacity) & m_mask), Capacity);
}

template<typename T, size_t Capacity>
bool ConcurrentRingBuffer<T, Capacity>::full() const noexcept
{
    const size_t position = m_head.load(std::memory_order_acquire);
    return position >= Capacity;
}

template<typename T, size_t Capacity>
size_t ConcurrentRingBuffer<T, Capacity>::size() const noexcept
{
    const size_t position = m_head.load(std::memory_order_acquire);
    return std::min(position, Capacity);
}

template<typename T, size_t Capacity>
constexpr size_t ConcurrentRingBuffer<T, Capacity>::capacity() noexcept
{
    return Capacity;
}

} // namespace nlx
#endif // !NLX_CORE_CONCURRENT_RING_BUFFER_H
