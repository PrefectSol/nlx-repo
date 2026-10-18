#ifndef NLX_CORE_CRYPTO_H
#define NLX_CORE_CRYPTO_H

#include <cstddef>
#include <string_view>

namespace nlx::crypto
{

inline constexpr size_t hmac_sha256_base64_size = 45;

void hmac_sha256_base64(char (&output)[hmac_sha256_base64_size], std::string_view key, std::string_view message) noexcept;

} // namespace nlx::crypto
#endif // !NLX_CORE_CRYPTO_H
