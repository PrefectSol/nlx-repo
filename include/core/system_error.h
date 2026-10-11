#ifndef NLX_CORE_SYSTEM_ERROR_H
#define NLX_CORE_SYSTEM_ERROR_H

#include <system_error>
#include <string>

namespace nlx 
{

class SystemError : public std::system_error
{
public:
    explicit SystemError(std::string message);

private:
    [[nodiscard]] static int capture_errno() noexcept;
};

} // namespace nlx
#endif // !NLX_CORE_SYSTEM_ERROR_H
