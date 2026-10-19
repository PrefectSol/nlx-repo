#include "parsing/env_reader.h"
#include "core/file.h"
#include "core/string.h"
#include "core/system_error.h"

#include <stdexcept>
#include <format>

namespace nlx::parsing
{

EnvReader::EnvReader(const char *path) :
    m_size(0), m_offset(0)
{
    const ssize_t size = file::read_file(path, m_data, kFileCapacity);
    if (size < 0) [[unlikely]]
    {
        const int error = errno;
        throw SystemError(error, "failed to read env file");
    }

    if (static_cast<size_t>(size) == kFileCapacity) [[unlikely]]
    {
        throw std::logic_error("env file is too large");
    }

    m_size = static_cast<size_t>(size);

    const ssize_t bom = file::detail::get_utf8bom_size(m_data, m_size);
    if (bom < 0) [[unlikely]]
    {
        throw std::runtime_error("env file has unsupported encoding");
    }

    m_offset = static_cast<size_t>(bom);
}

std::string_view EnvReader::get(std::string_view key) const
{
    if (key.empty())
    {
        throw std::invalid_argument(std::format("env key '{}' is empty", key));
    }

    const std::string_view content(m_data + m_offset, m_size - m_offset);
    const size_t size = content.size();

    bool found = false;
    std::string_view value;

    size_t pos = 0;
    while (pos < size)
    {
        size_t eol = content.find('\n', pos);
        if (eol == std::string_view::npos)
        {
            eol = size;
        }

        std::string_view line = content.substr(pos, eol - pos);
        pos = eol + 1;

        line = string::trim(line);
        if (line.empty() || line.front() == '#')
        {
            continue;
        }

        const size_t equal = line.find('=');
        if (equal == std::string_view::npos)
        {
            continue;
        }

        const std::string_view line_key = string::trim(line.substr(0, equal));
        if (!string::iequals(line_key, key))
        {
            continue;
        }

        value = string::trim(line.substr(equal + 1));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.front() == value.back())
        {
            value.remove_prefix(1);
            value.remove_suffix(1);
        }

        found = true;
    }

    if (!found)
    {
        throw std::invalid_argument(std::format("env key '{}' not found", key));
    }

    return value;
}

} // namespace nlx::parsing
