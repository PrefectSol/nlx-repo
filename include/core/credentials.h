#ifndef NLX_CORE_CREDENTIALS_H
#define NLX_CORE_CREDENTIALS_H

#include <cstddef>

namespace nlx
{

struct Credentials
{
    static constexpr size_t kApiFieldCapacity = 128;

    char api_key[kApiFieldCapacity];
    char api_secret[kApiFieldCapacity];
    char api_passphrase[kApiFieldCapacity];
    
    [[nodiscard]] static Credentials load(const char *path);
};

} // namespace nlx
#endif // !NLX_CORE_CREDENTIALS_H


