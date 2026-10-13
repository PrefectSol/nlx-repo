#include "core/system_error.h"

#include <cerrno>

namespace nlx
{

SystemError::SystemError(std::string message) :
    std::system_error(capture_errno(), std::system_category(), std::move(message))
{

}

SystemError::SystemError(int error, std::string message) :
    std::system_error(error, std::system_category(), std::move(message))
{

}

int SystemError::capture_errno() noexcept
{
    return errno;
}

} // namespace nlx
