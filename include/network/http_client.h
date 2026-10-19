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
    static constexpr int kAborted = -CURLE_ABORTED_BY_CALLBACK;
    static constexpr int kOverflow = -CURLE_WRITE_ERROR;

    char *buffer;
    size_t capacity;
    size_t size;
    int code;
    const FileDescriptor &notify;
};

class HttpClient
{
public:
    static constexpr size_t kTransfersSize = 8;
    static constexpr size_t kHeadersSize = 8;

    HttpClient() noexcept;
    ~HttpClient() noexcept;

    HttpClient(const HttpClient &) = delete;
    HttpClient &operator=(const HttpClient &) = delete;
    HttpClient(HttpClient &&) = delete;
    HttpClient &operator=(HttpClient &&) = delete;

    void initialize();

    void send(size_t index, HttpMethod method, const char *url, std::string_view body,
              std::span<const char *const> headers, HttpRequest &request) noexcept;
    void cancel(size_t index) noexcept;

    void perform(const FileDescriptor &stop, nanoseconds timeout) noexcept;
    [[nodiscard]] bool complete(size_t &index) noexcept;

    void wakeup() noexcept;

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

    void detach(Transfer &transfer) noexcept;

    template<typename T>
    static void set_option(CURL *handle, CURLoption option, T value) noexcept;

    static size_t write_response(char *data, size_t size, size_t count, void *context) noexcept;
};

} // namespace nlx::network
#endif // !NLX_NETWORK_HTTP_CLIENT_H
