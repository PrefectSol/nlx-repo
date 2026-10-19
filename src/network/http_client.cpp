#include "network/http_client.h"
#include "core/fatal.h"

#include <algorithm>
#include <climits>
#include <curl/curl.h>
#include <curl/multi.h>
#include <format>
#include <stdexcept>

namespace nlx::network
{

template<typename T>
void HttpClient::set_option(CURL *handle, CURLoption option, T value) noexcept
{
    if (curl_easy_setopt(handle, option, value) != CURLE_OK) [[unlikely]]
    {
        NLX_FATAL("nlx::network::HttpClient::set_option() curl_easy_setopt failed");
    }
}

HttpClient::HttpClient() noexcept :
    m_multi(nullptr),
    m_transfers()
{

}

HttpClient::~HttpClient() noexcept
{
    if (m_multi == nullptr)
    {
        return;
    }

    for (Transfer &transfer : m_transfers)
    {
        if (transfer.request != nullptr)
        {
            detach(transfer);
        }

        if (transfer.handle != nullptr)
        {
            curl_easy_cleanup(transfer.handle);
        }
    }

    curl_multi_cleanup(m_multi);
    curl_global_cleanup();
}

void HttpClient::initialize()
{
    if (m_multi != nullptr)
    {
        return;
    }

    const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (result != CURLE_OK)
    {
        throw std::runtime_error("curl_global_init failed");
    }

    m_multi = curl_multi_init();
    if (m_multi == nullptr)
    {
        curl_global_cleanup();
        throw std::runtime_error("curl_multi_init failed");
    }

    for (Transfer &transfer : m_transfers)
    {
        transfer.handle = curl_easy_init();
        if (transfer.handle == nullptr)
        {
            throw std::runtime_error("curl_easy_init failed");
        }

        const bool configure = curl_easy_setopt(transfer.handle, CURLOPT_WRITEFUNCTION, write_response) == CURLE_OK &&
                               curl_easy_setopt(transfer.handle, CURLOPT_WRITEDATA, &transfer) == CURLE_OK &&
                               curl_easy_setopt(transfer.handle, CURLOPT_CAINFO, CA_CERT_PATH) == CURLE_OK &&
                               curl_easy_setopt(transfer.handle, CURLOPT_TIMEOUT_MS, static_cast<long>(kTimeout.count())) == CURLE_OK &&
                               curl_easy_setopt(transfer.handle, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
                               curl_easy_setopt(transfer.handle, CURLOPT_TCP_NODELAY, 1L) == CURLE_OK;
        if (!configure)
        {
            throw std::runtime_error(std::format("curl_easy_setopt failed: {}", curl_easy_strerror(result)));
        }
    }
}

void HttpClient::send(size_t index, HttpMethod method, const char *url, std::string_view body,
                      std::span<const char *const> headers, HttpRequest &request) noexcept
{
    if (headers.size() > kHeadersSize) [[unlikely]]
    {
        NLX_FATAL("nlx::network::HttpClient::send() too many headers");
    }

    Transfer &transfer = m_transfers[index];
    transfer.request = &request;

    request.size = 0;
    request.code = 0;

    for (size_t i = 0; i < headers.size(); ++i)
    {
        curl_slist *next = i + 1 < headers.size() ? &transfer.headers[i + 1] : nullptr;
        transfer.headers[i] = { const_cast<char *>(headers[i]), next };
    }

    set_option(transfer.handle, CURLOPT_URL, url);
    set_option(transfer.handle, CURLOPT_HTTPHEADER, headers.empty() ? nullptr : transfer.headers);

    if (method == HttpMethod::Get)
    {
        set_option(transfer.handle, CURLOPT_HTTPGET, 1L);
    }
    else
    {
        set_option(transfer.handle, CURLOPT_POSTFIELDS, body.data());
        set_option(transfer.handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }

    const CURLMcode result = curl_multi_add_handle(m_multi, transfer.handle);
    if (result != CURLM_OK) [[unlikely]]
    {
        NLX_FATAL("nlx::network::HttpClient::send() curl_multi_add_handle failed");
    }
}

void HttpClient::cancel(size_t index) noexcept
{
    Transfer &transfer = m_transfers[index];
    if (transfer.request != nullptr)
    {
        detach(transfer);
    }
}

void HttpClient::perform(const FileDescriptor &stop, nanoseconds timeout) noexcept
{
    curl_waitfd extra{};
    extra.fd = stop.get();
    extra.events = CURL_WAIT_POLLIN;

    const int wait = static_cast<int>(std::min<long long>(detail::to_milliseconds(timeout).count(), INT_MAX));

    int running = 0;
    const CURLMcode polled = curl_multi_poll(m_multi, &extra, 1, wait, nullptr);
    const CURLMcode performed = curl_multi_perform(m_multi, &running);

    if (polled != CURLM_OK || performed != CURLM_OK) [[unlikely]]
    {
        NLX_FATAL("nlx::network::HttpClient::perform() curl_multi_perform failed");
    }
}

bool HttpClient::complete(size_t &index) noexcept
{
    int queued = 0;
    CURLMsg *message = curl_multi_info_read(m_multi, &queued);
    while (message != nullptr && message->msg != CURLMSG_DONE)
    {
        message = curl_multi_info_read(m_multi, &queued);
    }

    if (message == nullptr)
    {
        return false;
    }

    index = 0;
    while (m_transfers[index].handle != message->easy_handle)
    {
        ++index;
    }

    Transfer &transfer = m_transfers[index];
    HttpRequest &request = *transfer.request;

    const CURLcode result = message->data.result;
    if (result == CURLE_OK)
    {
        long status = 0;
        curl_easy_getinfo(transfer.handle, CURLINFO_RESPONSE_CODE, &status);
        request.code = static_cast<int>(status);
    }
    else
    {
        request.code = -static_cast<int>(result);
    }

    detach(transfer);

    return true;
}

void HttpClient::wakeup() noexcept
{
    curl_multi_wakeup(m_multi);
}

void HttpClient::detach(Transfer &transfer) noexcept
{
    const CURLMcode result = curl_multi_remove_handle(m_multi, transfer.handle);
    if (result != CURLM_OK) [[unlikely]]
    {
        NLX_FATAL("nlx::network::HttpClient::detach() curl_multi_remove_handle failed");
    }

    transfer.request = nullptr;
}

size_t HttpClient::write_response(char *data, size_t size, size_t count, void *context) noexcept
{
    const Transfer &transfer = *static_cast<const Transfer *>(context);
    HttpRequest &request = *transfer.request;

    const size_t bytes = size * count;
    if (bytes > request.capacity - request.size) [[unlikely]]
    {
        return 0;
    }

    std::copy_n(data, bytes, request.buffer + request.size);
    request.size += bytes;

    return bytes;
}

} // namespace nlx::network
