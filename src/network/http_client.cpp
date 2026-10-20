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

namespace 
{

template <typename T>
constexpr bool kSetOptArg = std::is_same_v<T, long> || std::is_same_v<T, curl_off_t> || std::is_pointer_v<T>;

template <typename T>
void easy_setopt(CURL *handle, CURLoption option, T value)
{
    static_assert(kSetOptArg<T>, "easy_setopt: long / curl_off_t / pointer");

    const CURLcode result = curl_easy_setopt(handle, option, value);
    if (result != CURLE_OK) [[unlikely]]
    {
        throw std::runtime_error(std::format("curl_easy_setopt failed: {}", curl_easy_strerror(result)));
    }
}

template <typename T>
void safe_easy_setopt(CURL *handle, CURLoption option, T value) noexcept
{
    static_assert(kSetOptArg<T>, "easy_setopt: long / curl_off_t / pointer");

    const CURLcode result = curl_easy_setopt(handle, option, value);
    if (result != CURLE_OK) [[unlikely]]
    {
        NLX_FATAL("nlx::network::safe_easy_setopt() curl_easy_setopt failed");
    }
}

template <typename T>
void multi_setopt(CURLM *multi, CURLMoption option, T value)
{
    static_assert(kSetOptArg<T>, "curl_multi_setopt: long / curl_off_t / pointer");
    
    const CURLMcode result = curl_multi_setopt(multi, option, value);
    if (result != CURLM_OK) [[unlikely]]
    {
        throw std::runtime_error(std::format("curl_multi_setopt failed: {}", curl_multi_strerror(result)));
    }
}

} // namespace

HttpClient::HttpClient():
    m_multi(nullptr),
    m_transfers()
{
    const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (result != CURLE_OK) [[unlikely]]
    {
        throw std::runtime_error("curl_global_init failed");
    }
}

HttpClient::~HttpClient() noexcept
{
    release();
    curl_global_cleanup();
}

void HttpClient::release() noexcept
{
    for (Transfer &transfer : m_transfers)
    {
        if (transfer.request != nullptr)
        {
            release_handle(transfer.handle);
            transfer.request = nullptr;
        }

        if (transfer.handle != nullptr)
        {
            curl_easy_cleanup(transfer.handle);
            transfer.handle = nullptr;
        }
    }

    if (m_multi != nullptr)
    {
        const CURLMcode result = curl_multi_cleanup(m_multi);
        if (result != CURLM_OK) [[unlikely]]
        {
            NLX_FATAL("nlx::network::HttpClient::release() curl_multi_cleanup failed");
        }

        m_multi = nullptr;
    }
}

void HttpClient::release_handle(CURL *handle) noexcept
{
    const CURLMcode result = curl_multi_remove_handle(m_multi, handle);
    if (result != CURLM_OK) [[unlikely]]
    {
        NLX_FATAL("nlx::network::HttpClient::release_handle() curl_multi_remove_handle failed");
    }
}

void HttpClient::initialize()
{
    if (m_multi != nullptr)
    {
        return;
    }

    try 
    {
        m_multi = curl_multi_init();
        if (m_multi == nullptr)
        {
            throw std::runtime_error("curl_multi_init failed");
        }

        multi_setopt(m_multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, static_cast<long>(kTransfersSize));
        multi_setopt(m_multi, CURLMOPT_MAXCONNECTS, static_cast<long>(kTransfersSize));

        for (Transfer &transfer : m_transfers)
        {
            transfer.handle = curl_easy_init();
            if (transfer.handle == nullptr)
            {
                throw std::runtime_error("curl_easy_init failed");
            }

            easy_setopt(transfer.handle, CURLOPT_WRITEFUNCTION, write_callback);
            easy_setopt(transfer.handle, CURLOPT_WRITEDATA, static_cast<void *>(&transfer));
            easy_setopt(transfer.handle, CURLOPT_PRIVATE, static_cast<void *>(&transfer));
            easy_setopt(transfer.handle, CURLOPT_CAINFO, CA_CERT_PATH);
            easy_setopt(transfer.handle, CURLOPT_TIMEOUT_MS, static_cast<long>(kTimeout.count()));
            easy_setopt(transfer.handle, CURLOPT_NOSIGNAL, 1L);
            easy_setopt(transfer.handle, CURLOPT_TCP_NODELAY, 1L);
        }
    }
    catch (...)
    {
        release();
        throw;
    }
}

void HttpClient::set_headers(size_t index, std::span<const char *const> headers)
{
    const size_t size = headers.size();
    if (headers.empty() || size > kHeadersSize) [[unlikely]]
    {
        throw std::invalid_argument("invalid http headers size");
    }

    Transfer &transfer = m_transfers[index];
    for (size_t i = 0; i < size; ++i)
    {
        curl_slist *next = i + 1 < size ? &transfer.headers[i + 1] : nullptr;
        transfer.headers[i] = { const_cast<char *>(headers[i]), next };
    }

    easy_setopt(transfer.handle, CURLOPT_HTTPHEADER, static_cast<curl_slist *>(transfer.headers));
}

void HttpClient::wakeup() noexcept
{
    if (m_multi == nullptr) [[unlikely]]
    {
        NLX_FATAL("nlx::network::HttpClient::wakeup() called before initialize()");
    }

    const CURLMcode result = curl_multi_wakeup(m_multi);
    if (result != CURLM_OK) [[unlikely]]
    {
        NLX_FATAL("nlx::network::HttpClient::wakeup() curl_multi_wakeup failed");
    }
}

size_t HttpClient::write_callback(char *data, size_t size, size_t nmemb, void *context) noexcept
{
    HttpRequest &request = *static_cast<const Transfer *>(context)->request;

    const size_t bytes = size * nmemb;
    const size_t available = std::min(bytes, request.capacity - request.size);

    std::copy_n(data, available, request.buffer + request.size);
    request.size += available;

    if (available < bytes)
    {
        request.truncated = true;
        return 0;
    }

    return bytes;
}

} // namespace nlx::network
