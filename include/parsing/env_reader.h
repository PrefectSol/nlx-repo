#ifndef NLX_PARSING_ENV_READER_H
#define NLX_PARSING_ENV_READER_H

#include <cstddef>
#include <string_view>

namespace nlx::parsing
{

class EnvReader
{
public:
    explicit EnvReader(const char *path);

    EnvReader(const EnvReader &) = delete;
    EnvReader &operator=(const EnvReader &) = delete;

    [[nodiscard]] std::string_view get(std::string_view key) const;
    [[nodiscard]] bool contains(std::string_view key) const noexcept;

private:
    bool find(std::string_view key, std::string_view &value) const noexcept;

    static constexpr size_t kFileCapacity = 1024;

    char m_data[kFileCapacity];
    size_t m_size;
    size_t m_offset;
};

} // namespace nlx::parsing
#endif // !NLX_PARSING_ENV_READER_H

