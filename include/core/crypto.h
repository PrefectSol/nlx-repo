#ifndef NLX_CORE_CRYPTO_H
#define NLX_CORE_CRYPTO_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace nlx::crypto
{

inline constexpr size_t hmac_sha256_base64_size = 45;

void hmac_sha256_base64(char (&output)[hmac_sha256_base64_size], std::string_view key, std::string_view message) noexcept;

[[nodiscard]] uint64_t fnv1a(std::string_view data) noexcept;

inline constexpr size_t aes_key_size = 32;
inline constexpr size_t gcm_nonce_size = 12;
inline constexpr size_t gcm_tag_size = 16;
inline constexpr size_t sealed_header_size = 8 + gcm_nonce_size + gcm_tag_size;

using AesKey = std::array<uint8_t, aes_key_size>;

[[nodiscard]] bool parse_key(std::string_view hex, AesKey &key) noexcept;
[[nodiscard]] bool random_bytes(std::span<uint8_t> output) noexcept;
[[nodiscard]] bool seal(std::span<const std::byte> plain, const AesKey &key, std::vector<std::byte> &sealed);
[[nodiscard]] bool open(std::span<const std::byte> sealed, const AesKey &key, std::vector<std::byte> &plain);

} // namespace nlx::crypto
#endif // !NLX_CORE_CRYPTO_H
