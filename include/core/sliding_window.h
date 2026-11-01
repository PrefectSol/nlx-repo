#ifndef NLX_CORE_SLIDING_WINDOW_H
#define NLX_CORE_SLIDING_WINDOW_H

#include <cstddef>
#include <span>
#include <type_traits>

namespace nlx
{

template<typename T, size_t Capacity>
class SlidingWindow
{
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::is_nothrow_copy_assignable_v<T>);
    static_assert(Capacity >= 1);

public:
    SlidingWindow() noexcept;

    SlidingWindow(const SlidingWindow &) = delete;
    SlidingWindow &operator=(const SlidingWindow &) = delete;
    SlidingWindow(SlidingWindow &&) = delete;
    SlidingWindow &operator=(SlidingWindow &&) = delete;

    void clear() noexcept;

    void push(const T &item) noexcept;
    void replace(const T &item) noexcept;

    [[nodiscard]] const T &back() const noexcept;
    [[nodiscard]] std::span<const T> history() const noexcept;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool full() const noexcept;
    [[nodiscard]] size_t size() const noexcept;

    [[nodiscard]] static constexpr size_t capacity() noexcept;

private:
    T m_data[2 * Capacity];
    size_t m_head;
    size_t m_size;
};

} // namespace nlx

namespace nlx
{

template<typename T, size_t Capacity>
SlidingWindow<T, Capacity>::SlidingWindow() noexcept :
    m_head(0),
    m_size(0)
{

}

template<typename T, size_t Capacity>
void SlidingWindow<T, Capacity>::clear() noexcept
{
    m_size = 0;
}

template<typename T, size_t Capacity>
void SlidingWindow<T, Capacity>::push(const T &item) noexcept
{
    m_data[m_head] = item;
    m_data[m_head + Capacity] = item;

    m_head = m_head + 1 == Capacity ? 0 : m_head + 1;
    m_size = m_size == Capacity ? Capacity : m_size + 1;
}

template<typename T, size_t Capacity>
void SlidingWindow<T, Capacity>::replace(const T &item) noexcept
{
    const size_t position = m_head == 0 ? Capacity - 1 : m_head - 1;

    m_data[position] = item;
    m_data[position + Capacity] = item;
}

template<typename T, size_t Capacity>
const T &SlidingWindow<T, Capacity>::back() const noexcept
{
    return m_data[m_head + Capacity - 1];
}

template<typename T, size_t Capacity>
std::span<const T> SlidingWindow<T, Capacity>::history() const noexcept
{
    return std::span<const T>(m_data + m_head + Capacity - m_size, m_size);
}

template<typename T, size_t Capacity>
bool SlidingWindow<T, Capacity>::empty() const noexcept
{
    return m_size == 0;
}

template<typename T, size_t Capacity>
bool SlidingWindow<T, Capacity>::full() const noexcept
{
    return m_size == Capacity;
}

template<typename T, size_t Capacity>
size_t SlidingWindow<T, Capacity>::size() const noexcept
{
    return m_size;
}

template<typename T, size_t Capacity>
constexpr size_t SlidingWindow<T, Capacity>::capacity() noexcept
{
    return Capacity;
}

} // namespace nlx
#endif // !NLX_CORE_SLIDING_WINDOW_H
