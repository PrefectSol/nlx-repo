#ifndef NLX_CORE_STRING_H
#define NLX_CORE_STRING_H

#include <cassert>
#include <cstddef>
#include <string_view>
#include <cstring>
#include <algorithm> 

namespace nlx::string 
{

template <size_t Size>
[[nodiscard]] constexpr bool safe_strcpy(char (&dest)[Size], std::string_view src) noexcept;

template <size_t Capacity>
class BoundedLine
{
public:
    BoundedLine() noexcept;

    void append(std::string_view text) noexcept;
    [[nodiscard]] std::string_view finish() noexcept;
    void clear() noexcept;

private:
    static constexpr char kOverflowMarker[] = "(overflow)";
    static constexpr size_t kReserve = sizeof(kOverflowMarker);

    char m_data[Capacity + kReserve];
    size_t m_size;
    bool m_overflow;
};

} // namespace nlx::string

namespace nlx::detail
{

template <size_t Size>
constexpr void str_assign(char (&dest)[Size], std::string_view src) noexcept;

[[nodiscard]] consteval size_t sizeof_dateformat(std::string_view format);

template<size_t N>
[[nodiscard]] consteval size_t length_dateformat(const char (&format)[N]) noexcept;

} // namespace nlx::detail

template <size_t Size>
constexpr bool nlx::string::safe_strcpy(char (&dest)[Size], std::string_view src) noexcept
{
    if (src.size() >= Size) [[unlikely]]
    {
        return false; 
    }

    if (src.empty())
    {
        dest[0] = '\0';
        return true;
    }

    std::copy_n(src.data(), src.size(), dest);
    dest[src.size()] = '\0';

    return true;
}

template <size_t Size>
constexpr void nlx::detail::str_assign(char (&dest)[Size], std::string_view src) noexcept
{
    [[maybe_unused]] const bool result = string::safe_strcpy(dest, src);
    assert(result);
}

consteval size_t nlx::detail::sizeof_dateformat(std::string_view format)
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
consteval size_t nlx::detail::length_dateformat(const char (&format)[N]) noexcept
{
    const std::string_view date = std::string_view(format).substr(2, N - 4);
    return sizeof_dateformat(date);
}

template <size_t Capacity>
nlx::string::BoundedLine<Capacity>::BoundedLine() noexcept :
    m_size(0), m_overflow(false)
{

}

template <size_t Capacity>
void nlx::string::BoundedLine<Capacity>::append(std::string_view text) noexcept
{
    const size_t available = Capacity - m_size;
    const size_t length = std::min(text.size(), available);

    std::copy_n(text.data(), length, m_data + m_size);
    m_size += length;

    if (length != text.size())
    {
        m_overflow = true;
    }
}

template <size_t Capacity>
std::string_view nlx::string::BoundedLine<Capacity>::finish() noexcept
{
    if (m_overflow)
    {
        std::copy_n(kOverflowMarker, kReserve - 1, m_data + m_size);
        m_size += kReserve - 1;
    }

    m_data[m_size] = '\n';
    return std::string_view(m_data, m_size + 1);
}

template<size_t Capacity>
void nlx::string::BoundedLine<Capacity>::clear() noexcept
{
    m_size = 0;
    m_overflow = false;
}

#endif // !NLX_CORE_STRING_H
