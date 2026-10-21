#include "network/http_client.h"
#include "core/fatal.h"

#include <algorithm>
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

CURLM *make_multi(long connections) noexcept
{
    CURLM *multi = curl_multi_init();
    if (multi == nullptr) [[unlikely]]
    {
        return nullptr;
    }

    const bool configured = curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, connections) == CURLM_OK &&
                            curl_multi_setopt(multi, CURLMOPT_MAXCONNECTS, connections) == CURLM_OK;
    if (!configured) [[unlikely]]
    {
        curl_multi_cleanup(multi);
        return nullptr;
    }

    return multi;
}

nanoseconds info_time(CURL *handle, CURLINFO info) noexcept
{
    curl_off_t value = 0;
    curl_easy_getinfo(handle, info, &value);

    return microseconds(value);
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
    destroy();
    curl_global_cleanup();
}

void HttpClient::initialize()
{
    if (m_multi != nullptr)
    {
        return;
    }

    try
    {
        m_multi = make_multi(static_cast<long>(kTransfersSize));
        if (m_multi == nullptr)
        {
            throw std::runtime_error("curl_multi_init failed");
        }

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
            easy_setopt(transfer.handle, CURLOPT_ERRORBUFFER, static_cast<char *>(transfer.error));
            easy_setopt(transfer.handle, CURLOPT_PREREQFUNCTION, prereq_callback);
            easy_setopt(transfer.handle, CURLOPT_PREREQDATA, static_cast<void *>(&transfer));
            easy_setopt(transfer.handle, CURLOPT_CAINFO, CA_CERT_PATH);
            easy_setopt(transfer.handle, CURLOPT_TIMEOUT_MS, static_cast<long>(kTimeout.count()));
            easy_setopt(transfer.handle, CURLOPT_NOSIGNAL, 1L);
            easy_setopt(transfer.handle, CURLOPT_TCP_NODELAY, 1L);
        }
    }
    catch (...)
    {
        destroy();
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

void HttpClient::launch(size_t index, bool post, const char *url, std::string_view body, HttpRequest &request) noexcept
{
    Transfer &transfer = m_transfers[index];
    CURL *handle = transfer.handle;

    safe_easy_setopt(handle, CURLOPT_URL, url);

    if (post)
    {
        safe_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        safe_easy_setopt(handle, CURLOPT_POSTFIELDS, body.data());
    }
    else
    {
        safe_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
    }

    transfer.request = &request;
    transfer.error[0] = '\0';
    transfer.sent = false;

    const CURLMcode result = curl_multi_add_handle(m_multi, handle);
    if (result != CURLM_OK) [[unlikely]]
    {
        NLX_FATAL("nlx::network::HttpClient::launch() curl_multi_add_handle failed");
    }
}

void HttpClient::perform() noexcept
{
    int running = 0;

    const CURLMcode result = curl_multi_perform(m_multi, &running);
    if (result != CURLM_OK) [[unlikely]]
    {
        NLX_FATAL("nlx::network::HttpClient::perform() curl_multi_perform failed");
    }
}

bool HttpClient::read(size_t &index, HttpResult &result) noexcept
{
    int queued = 0;

    while (CURLMsg *message = curl_multi_info_read(m_multi, &queued))
    {
        if (message->msg != CURLMSG_DONE)
        {
            continue;
        }

        CURL *handle = message->easy_handle;
        const CURLcode code = message->data.result;

        void *context = nullptr;
        curl_easy_getinfo(handle, CURLINFO_PRIVATE, &context);

        Transfer &transfer = *static_cast<Transfer *>(context);
        HttpRequest &request = *transfer.request;

        long status = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);

        const bool received = (code == CURLE_OK || request.truncated) && status != 0;
        request.code = received ? static_cast<int>(status) : -1;

        result.error = static_cast<int>(code);
        result.status = static_cast<int>(status);
        result.sent = transfer.sent;
        result.message = transfer.error[0] != '\0' ? transfer.error : curl_easy_strerror(code);

        result.elapsed = info_time(handle, CURLINFO_TOTAL_TIME_T);
        result.resolve = info_time(handle, CURLINFO_NAMELOOKUP_TIME_T);
        result.connect = info_time(handle, CURLINFO_CONNECT_TIME_T);
        result.handshake = info_time(handle, CURLINFO_APPCONNECT_TIME_T);

        release_handle(handle);
        transfer.request = nullptr;

        index = static_cast<size_t>(&transfer - m_transfers.data());

        return true;
    }

    return false;
}

bool HttpClient::poll(milliseconds timeout, const FileDescriptor &fd) noexcept
{
    curl_waitfd extra{ fd.get(), CURL_WAIT_POLLIN, 0 };

    const CURLMcode result = curl_multi_poll(m_multi, &extra, 1, static_cast<int>(timeout.count()), nullptr);
    if (result != CURLM_OK) [[unlikely]]
    {
        NLX_FATAL("nlx::network::HttpClient::poll() curl_multi_poll failed");
    }

    return (extra.revents & CURL_WAIT_POLLIN) != 0;
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

void HttpClient::release() noexcept
{
    for (Transfer &transfer : m_transfers)
    {
        if (transfer.request != nullptr)
        {
            release_handle(transfer.handle);
            transfer.request = nullptr;
        }
    }
}

void HttpClient::destroy() noexcept
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
            NLX_FATAL("nlx::network::HttpClient::destroy() curl_multi_cleanup failed");
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

int HttpClient::prereq_callback(void *context, char *primary_ip, char *local_ip, int primary_port, int local_port) noexcept
{
    static_cast<Transfer *>(context)->sent = true;
    return CURL_PREREQFUNC_OK;
}

} // namespace nlx::network
