#ifndef NLX_CORE_SYSTEM_ERROR_H
#define NLX_CORE_SYSTEM_ERROR_H

#include <string>
#include <system_error>

namespace nlx 
{

class SystemError : public std::system_error
{
public:
    explicit SystemError(std::string message);
    explicit SystemError(int error, std::string message);

private:
    static int capture_errno() noexcept;
};

} // namespace nlx
#endif // !NLX_CORE_SYSTEM_ERROR_H
