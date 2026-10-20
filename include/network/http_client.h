#ifndef NLX_NETWORK_HTTP_CLIENT_H
#define NLX_NETWORK_HTTP_CLIENT_H

#include "core/file_descriptor.h"
#include "core/time.h"

#include <curl/curl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace nlx::network
{

enum class HttpMethod : uint8_t
{
    Get,
    Post
};

struct HttpRequest
{
    char *buffer;
    size_t capacity;
    size_t size;
    int code;
    bool truncated;
    const FileDescriptor &notify;
};

class HttpClient
{
public:
    HttpClient();
    ~HttpClient() noexcept;

    HttpClient(const HttpClient &) = delete;
    HttpClient &operator=(const HttpClient &) = delete;
    HttpClient(HttpClient &&) = delete;
    HttpClient &operator=(HttpClient &&) = delete;

    void initialize();
    void set_headers(size_t index, std::span<const char *const> headers); 

    void wakeup() noexcept;

    static constexpr size_t kTransfersSize = sizeof(uint8_t) * 8;
    static constexpr size_t kHeadersSize = 8;

private:
    static constexpr milliseconds kTimeout{ 5000 };

    struct Transfer
    {
        CURL *handle;
        HttpRequest *request;
        curl_slist headers[kHeadersSize];
    };

    CURLM *m_multi;
    std::array<Transfer, kTransfersSize> m_transfers;

    void release() noexcept;
    void release_handle(CURL *handle) noexcept;

    static size_t write_callback(char *data, size_t size, size_t nmemb, void *context) noexcept;
};

} // nlx::network
#endif // !NLX_NETWORK_HTTP_CLIENT_H
