#ifndef NLX_NETWORK_HTTP_CLIENT_H
#define NLX_NETWORK_HTTP_CLIENT_H

#include "core/file_descriptor.h"
#include "core/time.h"

#include <curl/curl.h>

#include <cstddef>
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

class HttpClient
{
public:
    HttpClient();
    ~HttpClient() noexcept;

    HttpClient(const HttpClient &) = delete;
    HttpClient &operator=(const HttpClient &) = delete;
    HttpClient(HttpClient &&) = delete;
    HttpClient &operator=(HttpClient &&) = delete;

    [[nodiscard]] CURL *open();
    void close(CURL *handle) noexcept;

    void send(CURL *handle, const char *url, std::string_view body, curl_slist *headers, HttpRequest &request) noexcept;
    void cancel(CURL *handle) noexcept;

    void perform(nanoseconds timeout) noexcept;
    [[nodiscard]] HttpRequest *complete(CURL *&handle) noexcept;

    void wakeup() noexcept;

private:
    static constexpr long kTimeout = 5000;

    CURLM *m_multi;

    static size_t on_write(char *data, size_t size, size_t count, void *context) noexcept;
};

} // namespace nlx::network
#endif // !NLX_NETWORK_HTTP_CLIENT_H
