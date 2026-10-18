#include "core/crypto.h"
#include "core/fatal.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace nlx::crypto
{

static_assert(hmac_sha256_base64_size == 4 * ((SHA256_DIGEST_LENGTH + 2) / 3) + 1);

void hmac_sha256_base64(char (&output)[hmac_sha256_base64_size], std::string_view key, std::string_view message) noexcept
{
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

} // namespace nlx::crypto
