#include "core/system_error.h"

namespace nlx
{

SystemError::SystemError(std::string message) :
    std::system_error(capture_errno(), std::system_category(), std::move(message))
{

}

int SystemError::capture_errno() noexcept
{
    return errno;
}

} // namespace nlx
