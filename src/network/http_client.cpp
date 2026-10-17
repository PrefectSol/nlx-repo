#include "network/http_client.h"
#include "core/fatal.h"

#include <algorithm>
#include <climits>
#include <stdexcept>

namespace nlx::network
{

namespace
{

HttpRequest *get_request(CURL *handle) noexcept
{
    void *request = nullptr;
    curl_easy_getinfo(handle, CURLINFO_PRIVATE, &request);

    return static_cast<HttpRequest *>(request);
}

void detach(CURLM *multi, CURL *handle) noexcept
{
    const CURLMcode removed = curl_multi_remove_handle(multi, handle);
    const CURLcode cleared = curl_easy_setopt(handle, CURLOPT_PRIVATE, nullptr);

    if (removed != CURLM_OK || cleared != CURLE_OK) [[unlikely]]
    {
        NLX_FATAL("nlx::network::detach() curl_multi_remove_handle failed");
    }
}

} // namespace

HttpClient::HttpClient() :
    m_multi(nullptr)
{
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    {
        throw std::runtime_error("curl_global_init failed");
    }

    m_multi = curl_multi_init();
    if (m_multi == nullptr)
    {
        curl_global_cleanup();
        throw std::runtime_error("curl_multi_init failed");
    }
}

HttpClient::~HttpClient() noexcept
{
    curl_multi_cleanup(m_multi);
    curl_global_cleanup();
}

CURL *HttpClient::open()
{
    CURL *handle = curl_easy_init();
    if (handle == nullptr)
    {
        throw std::runtime_error("curl_easy_init failed");
    }

    const bool configured = curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, static_cast<curl_write_callback>(on_write)) == CURLE_OK &&
                            curl_easy_setopt(handle, CURLOPT_CAINFO, CA_CERT_PATH) == CURLE_OK &&
                            curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, kTimeout) == CURLE_OK &&
                            curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
                            curl_easy_setopt(handle, CURLOPT_TCP_NODELAY, 1L) == CURLE_OK;
    if (!configured)
    {
        curl_easy_cleanup(handle);
        throw std::runtime_error("curl_easy_setopt failed");
    }

    return handle;
}

void HttpClient::close(CURL *handle) noexcept
{
    cancel(handle);
    curl_easy_cleanup(handle);
}

void HttpClient::send(CURL *handle, const char *url, std::string_view body, curl_slist *headers, HttpRequest &request) noexcept
{
    request.size = 0;
    request.code = 0;
    request.truncated = false;

    bool method = false;
    if (body.empty())
    {
        method = curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L) == CURLE_OK;
    }
    else
    {
        method = curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.data()) == CURLE_OK &&
                 curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size())) == CURLE_OK;
    }

    const bool sent = method &&
                      curl_easy_setopt(handle, CURLOPT_URL, url) == CURLE_OK &&
                      curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers) == CURLE_OK &&
                      curl_easy_setopt(handle, CURLOPT_PRIVATE, &request) == CURLE_OK &&
                      curl_easy_setopt(handle, CURLOPT_WRITEDATA, &request) == CURLE_OK &&
                      curl_multi_add_handle(m_multi, handle) == CURLM_OK;

    if (!sent) [[unlikely]]
    {
        NLX_FATAL("nlx::network::HttpClient::send() curl_multi_add_handle failed");
    }
}

void HttpClient::cancel(CURL *handle) noexcept
{
    if (get_request(handle) != nullptr)
    {
        detach(m_multi, handle);
    }
}

void HttpClient::perform(nanoseconds timeout) noexcept
{
    const long long timeout_ms = std::min<long long>(detail::to_milliseconds(timeout).count(), INT_MAX);

    int running = 0;
    const bool performed = curl_multi_poll(m_multi, nullptr, 0, static_cast<int>(timeout_ms), nullptr) == CURLM_OK &&
                           curl_multi_perform(m_multi, &running) == CURLM_OK;

    if (!performed) [[unlikely]]
    {
        NLX_FATAL("nlx::network::HttpClient::perform() curl_multi_perform failed");
    }
}

HttpRequest *HttpClient::complete(CURL *&handle) noexcept
{
    int queued = 0;
    CURLMsg *message = curl_multi_info_read(m_multi, &queued);
    while (message != nullptr && message->msg != CURLMSG_DONE)
    {
        message = curl_multi_info_read(m_multi, &queued);
    }

    if (message == nullptr)
    {
        return nullptr;
    }

    handle = message->easy_handle;
    HttpRequest *request = get_request(handle);

    const CURLcode result = message->data.result;
    if (result == CURLE_OK)
    {
        long status = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        request->code = static_cast<int>(status);
    }
    else
    {
        request->code = -static_cast<int>(result);
    }

    detach(m_multi, handle);

    return request;
}

void HttpClient::wakeup() noexcept
{
    curl_multi_wakeup(m_multi);
}

size_t HttpClient::on_write(char *data, size_t size, size_t count, void *context) noexcept
{
    HttpRequest *request = static_cast<HttpRequest *>(context);

    const size_t bytes = size * count;
    const size_t length = std::min(bytes, request->capacity - request->size);

    std::copy_n(data, length, request->buffer + request->size);
    request->size += length;

    if (length != bytes) [[unlikely]]
    {
        request->truncated = true;
    }

    return bytes;
}

} // namespace nlx::network
