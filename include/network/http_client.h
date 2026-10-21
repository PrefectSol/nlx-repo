#ifndef NLX_NETWORK_HTTP_CLIENT_H
#define NLX_NETWORK_HTTP_CLIENT_H

#include "core/file_descriptor.h"
#include "core/time.h"

#include <curl/curl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace nlx::network
{

struct HttpRequest
{
    char *buffer;
    size_t capacity;
    size_t size;
    int code;
    bool truncated;
    const FileDescriptor &notify;
};

struct HttpResult
{
    int error;
    int status;
    bool sent;
    const char *message;

    nanoseconds elapsed;
    nanoseconds resolve;
    nanoseconds connect;
    nanoseconds handshake;
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

    void launch(size_t index, bool post, const char *url, std::string_view body, HttpRequest &request) noexcept;
    void perform() noexcept;
    [[nodiscard]] bool read(size_t &index, HttpResult &result) noexcept;
    [[nodiscard]] bool poll(milliseconds timeout, const FileDescriptor &fd) noexcept;

    void wakeup() noexcept;
    void release() noexcept;

    using SlotMask = uint8_t;

    static constexpr size_t kTransfersSize = std::numeric_limits<SlotMask>::digits;
    static constexpr size_t kHeadersSize = 8;

private:
    static constexpr milliseconds kTimeout{ 5000 };

    struct Transfer
    {
        CURL *handle;
        HttpRequest *request;
        curl_slist headers[kHeadersSize];
        char error[CURL_ERROR_SIZE];
        bool sent;
    };

    CURLM *m_multi;
    std::array<Transfer, kTransfersSize> m_transfers;

    void destroy() noexcept;
    void release_handle(CURL *handle) noexcept;

    static size_t write_callback(char *data, size_t size, size_t nmemb, void *context) noexcept;
    static int prereq_callback(void *context, char *primary_ip, char *local_ip, int primary_port, int local_port) noexcept;
};

} // nlx::network
#endif // !NLX_NETWORK_HTTP_CLIENT_H
