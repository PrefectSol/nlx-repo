#ifndef NLX_CORE_STRING_H
#define NLX_CORE_STRING_H

#include <cassert>
#include <cstddef>
#include <string_view>
#include <cstring>
#include <algorithm> 
#include <charconv>

namespace nlx::string 
{

template <size_t N>
[[nodiscard]] constexpr bool safe_strcpy(char (&dest)[N], std::string_view src) noexcept;

template <size_t Capacity>
class BoundedLine
{
public:
    BoundedLine() noexcept;

    void append(std::string_view text) noexcept;

    template <typename T>
        requires std::integral<T> || std::floating_point<T>
    void append(T value) noexcept;

    [[nodiscard]] std::string_view finish() noexcept;
    void clear() noexcept;

private:
    static constexpr char kOverflowMarker[] = "(overflow)";
    static constexpr size_t kReserve = sizeof(kOverflowMarker);

    char m_data[Capacity + kReserve];
    size_t m_size;
    bool m_overflow;
};

template<size_t Capacity, typename ...Args>
class BasicFormatString
{
public:
    template<size_t N>
    consteval BasicFormatString(const char (&str)[N]) noexcept;

    [[nodiscard]] constexpr std::string_view get() const noexcept;

private:
    std::string_view m_view;
};

} // namespace nlx::string

namespace nlx::detail
{

template <size_t N>
constexpr void str_assign(char (&dest)[N], std::string_view src) noexcept;

[[nodiscard]] consteval size_t sizeof_dateformat(std::string_view format) noexcept;

template<size_t N>
[[nodiscard]] consteval size_t length_dateformat(const char (&format)[N]) noexcept;

void format_error(const char *message) noexcept;
[[nodiscard]] consteval size_t count_placeholders(std::string_view format) noexcept;

} // namespace nlx::detail

namespace nlx::string 
{

template <size_t N>
constexpr bool safe_strcpy(char (&dest)[N], std::string_view src) noexcept
{
    const size_t size = src.size();
    if (size >= N) [[unlikely]]
    {
        return false; 
    }

    if (size == 0)
    {
        dest[0] = '\0';
        return true;
    }

    std::copy_n(src.data(), size, dest);
    dest[size] = '\0';

    return true;
}

template <size_t Capacity>
BoundedLine<Capacity>::BoundedLine() noexcept :
    m_size(0), m_overflow(false)
{

}

template <size_t Capacity>
void BoundedLine<Capacity>::append(std::string_view text) noexcept
{
    if (m_size >= Capacity) [[unlikely]]
    {
        m_overflow = true;
        return;
    }

    const size_t available = Capacity - m_size;
    const size_t size = text.size();
    const size_t length = std::min(size, available);

    std::copy_n(text.data(), length, m_data + m_size);
    m_size += length;

    if (length != size)
    {
        m_overflow = true;
    }
}

template <size_t Capacity>
template <typename T>
    requires std::integral<T> || std::floating_point<T>
void BoundedLine<Capacity>::append(T value) noexcept
{
    if (m_size >= Capacity) [[unlikely]]
    {
        m_overflow = true;
        return;
    }

    const size_t available = Capacity - m_size;
    const auto result = std::to_chars(m_data + m_size, m_data + m_size + available, value);
    if (result.ec != std::errc()) [[unlikely]]
    {
        m_overflow = true;
        return;
    }
        
    m_size += static_cast<size_t>(result.ptr - (m_data + m_size));
}

template <size_t Capacity>
std::string_view BoundedLine<Capacity>::finish() noexcept
{
    if (m_overflow)
    {
        std::copy_n(kOverflowMarker, kReserve - 1, m_data + m_size);
        m_size += kReserve - 1;
    }

    m_data[m_size++] = '\n';
    return std::string_view(m_data, m_size);
}

template<size_t Capacity>
void BoundedLine<Capacity>::clear() noexcept
{
    m_size = 0;
    m_overflow = false;
}

template<size_t Capacity, typename ...Args>
template<size_t N>
consteval BasicFormatString<Capacity, Args...>::BasicFormatString(const char (&str)[N]) noexcept
    : m_view(str, N - 1)
{
    static_assert(N - 1 <= Capacity, "format string is too long");

    if (detail::count_placeholders(m_view) != sizeof...(Args))
    {
        detail::format_error("placeholders count does not match arguments count");
    }
}

template<size_t Capacity, typename ...Args>
constexpr std::string_view BasicFormatString<Capacity, Args...>::get() const noexcept
{
    return m_view;
}

} // namespace nlx::string

namespace nlx::detail
{

template <size_t N>
constexpr void str_assign(char (&dest)[N], std::string_view src) noexcept
{
    [[maybe_unused]] const bool result = string::safe_strcpy(dest, src);
    assert(result);
}

consteval size_t sizeof_dateformat(std::string_view format) noexcept
{
    size_t count = 0;

    const size_t size = format.size();
    for (size_t i = 0; i < size; ++i)
    {
        if (format[i] != '%')
        {
            ++count;
            continue;
        }

        ++i;
        if (i >= size)    
        {
            break;
        }
        
        if (format[i] == 'E' || format[i] == 'O')
        {
            ++i;
            if (i >= size)
            {
                break;
            }
        }

        switch (format[i])
        {
            case 'F': count += 10; break;
            case 'T': count += 18; break;
            case 'R': count += 5;  break;
            case 'r': count += 11; break;
            case 'D': count += 8;  break;
            case 'Y': count += 4;  break;
            case 'y': count += 2;  break;
            case 'C': count += 2;  break;
            case 'm': count += 2;  break;
            case 'b':
            case 'h': count += 3;  break;
            case 'B': count += 9;  break;
            case 'd':
            case 'e': count += 2;  break;
            case 'a': count += 3;  break;
            case 'A': count += 9;  break;
            case 'w':
            case 'u': count += 1;  break;
            case 'H':
            case 'I': count += 2;  break;
            case 'M': count += 2;  break;
            case 'S': count += 2;  break;
            case 'p': count += 2;  break;
            case 'j': count += 3;  break;
            case 'U': 
            case 'W':
            case 'V': count += 2;  break;
            case 'G': count += 4;  break;
            case 'g': count += 2;  break;
            case 'z': count += 6;  break;
            case 'Z': count += 6;  break;
            case 'n':
            case 't': count += 1;  break;
            case '%': count += 1;  break;
            default:  count += 16; break;
        }
    }

    return count;
}

template<size_t N>
consteval size_t length_dateformat(const char (&format)[N]) noexcept
{
    static_assert(N > 5);

    const std::string_view date = std::string_view(format).substr(2, N - 4);
    return sizeof_dateformat(date);
}

consteval size_t count_placeholders(std::string_view format) noexcept
{
    size_t count = 0;

    const size_t size = format.size();
    for (size_t i = 0; i < size; ++i)
    {
        const char current = format[i];
        const char next = i + 1 < size ? format[i + 1] : '\0';

        if (current == '{')
        {
            if (next == '{')
            {
                ++i;
            }
            else if (next == '}')
            {
                ++count;
                ++i;
            }
            else
            {
                format_error("unmatched '{' in format string");
            }
        }
        else if (current == '}')
        {
            if (next != '}')
            {
                format_error("unmatched '}' in format string");
            }

            ++i;
        }
    }

    return count;
}

} // namespace nlx::detail
#endif // !NLX_CORE_STRING_H
