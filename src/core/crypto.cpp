#include "core/crypto.h"
#include "core/fatal.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cstring>
#include <memory>

namespace nlx::crypto
{

void hmac_sha256_base64(char (&output)[hmac_sha256_base64_size], std::string_view key, std::string_view message) noexcept
{
    static_assert(hmac_sha256_base64_size == 4 * ((SHA256_DIGEST_LENGTH + 2) / 3) + 1);

    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int size = 0;

    const unsigned char *data = reinterpret_cast<const unsigned char *>(message.data());
    const unsigned char *result = HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
                                       data, message.size(), digest, &size);

    if (result == nullptr) [[unlikely]]
    {
        NLX_FATAL("nlx::crypto::hmac_sha256_base64() HMAC failed");
    }

    EVP_EncodeBlock(reinterpret_cast<unsigned char *>(output), digest, static_cast<int>(size));
}

uint64_t fnv1a(std::string_view data) noexcept
{
    uint64_t hash = 14695981039346656037ULL;
    for (char ch : data)
    {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ULL;
    }

    return hash;
}

namespace
{

constexpr char kSealedMagic[] = "NLXE";
constexpr uint32_t kSealedVersion = 1;

struct CipherRelease
{
    void operator()(EVP_CIPHER_CTX *context) const noexcept
    {
        EVP_CIPHER_CTX_free(context);
    }
};

using Cipher = std::unique_ptr<EVP_CIPHER_CTX, CipherRelease>;

int hex_digit(char symbol) noexcept
{
    if (symbol >= '0' && symbol <= '9')
    {
        return symbol - '0';
    }

    if (symbol >= 'a' && symbol <= 'f')
    {
        return symbol - 'a' + 10;
    }

    if (symbol >= 'A' && symbol <= 'F')
    {
        return symbol - 'A' + 10;
    }

    return -1;
}

} // namespace

bool parse_key(std::string_view hex, AesKey &key) noexcept
{
    if (hex.size() != 2 * aes_key_size)
    {
        return false;
    }

    for (size_t index = 0; index < aes_key_size; ++index)
    {
        const int high = hex_digit(hex[2 * index]);
        const int low = hex_digit(hex[2 * index + 1]);

        if (high < 0 || low < 0)
        {
            return false;
        }

        key[index] = static_cast<uint8_t>(high * 16 + low);
    }

    return true;
}

bool random_bytes(std::span<uint8_t> output) noexcept
{
    return RAND_bytes(output.data(), static_cast<int>(output.size())) == 1;
}

bool seal(std::span<const std::byte> plain, const AesKey &key, std::vector<std::byte> &sealed)
{
    sealed.assign(sealed_header_size + plain.size(), std::byte{});

    uint8_t *const header = reinterpret_cast<uint8_t *>(sealed.data());
    std::memcpy(header, kSealedMagic, 4);
    std::memcpy(header + 4, &kSealedVersion, sizeof(kSealedVersion));

    uint8_t *const nonce = header + 8;
    uint8_t *const tag = nonce + gcm_nonce_size;
    uint8_t *const body = tag + gcm_tag_size;

    if (!random_bytes({ nonce, gcm_nonce_size }))
    {
        return false;
    }

    const Cipher context(EVP_CIPHER_CTX_new());

    int length = 0;
    bool result = context != nullptr;
    result = result && EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, key.data(), nonce) == 1;
    result = result && EVP_EncryptUpdate(context.get(), nullptr, &length, header, 8) == 1;
    result = result && EVP_EncryptUpdate(context.get(), body, &length, reinterpret_cast<const uint8_t *>(plain.data()), static_cast<int>(plain.size())) == 1;
    result = result && EVP_EncryptFinal_ex(context.get(), body + length, &length) == 1;
    result = result && EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(gcm_tag_size), tag) == 1;

    return result;
}

bool open(std::span<const std::byte> sealed, const AesKey &key, std::vector<std::byte> &plain)
{
    if (sealed.size() < sealed_header_size)
    {
        return false;
    }

    const uint8_t *const header = reinterpret_cast<const uint8_t *>(sealed.data());

    uint32_t version = 0;
    std::memcpy(&version, header + 4, sizeof(version));

    if (std::memcmp(header, kSealedMagic, 4) != 0 || version != kSealedVersion)
    {
        return false;
    }

    const uint8_t *const nonce = header + 8;
    const uint8_t *const tag = nonce + gcm_nonce_size;
    const uint8_t *const body = tag + gcm_tag_size;
    const size_t size = sealed.size() - sealed_header_size;

    plain.assign(size, std::byte{});

    uint8_t expected[gcm_tag_size];
    std::memcpy(expected, tag, gcm_tag_size);

    const Cipher context(EVP_CIPHER_CTX_new());

    int length = 0;
    bool result = context != nullptr;
    result = result && EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, key.data(), nonce) == 1;
    result = result && EVP_DecryptUpdate(context.get(), nullptr, &length, header, 8) == 1;
    result = result && EVP_DecryptUpdate(context.get(), reinterpret_cast<uint8_t *>(plain.data()), &length, body, static_cast<int>(size)) == 1;
    result = result && EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(gcm_tag_size), expected) == 1;
    result = result && EVP_DecryptFinal_ex(context.get(), reinterpret_cast<uint8_t *>(plain.data()) + length, &length) == 1;

    if (!result)
    {
        plain.clear();
    }

    return result;
}

} // namespace nlx::crypto
