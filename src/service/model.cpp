#include "service/model.h"
#include "core/fatal.h"

#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <vector>

namespace nlx::service
{

Model::Model(const Credentials &credentials, logging::LoggerRegistry &registry) :
    m_log(),
    m_network(),
    m_key(),
    m_keyed(false),
    m_epoll(),
    m_stop(),
    m_request(),
    m_timer(),
    m_initialized(false),
    m_ready(false),
    m_requests(),
    m_loop(),
    m_inference(),
    m_waiting()
{
    m_log = registry.create_node("model");

    m_keyed = crypto::parse_key(credentials.model_key, m_key);

    m_epoll = FileDescriptor::make_epoll();
    m_stop = FileDescriptor::make_eventfd(true);
    m_request = FileDescriptor::make_eventfd(true);
    m_timer = FileDescriptor::make_timerfd(milliseconds(kMetricsMilliseconds), true);

    detail::epoll_add(m_epoll, m_stop);
    detail::epoll_add(m_epoll, m_request);
    detail::epoll_add(m_epoll, m_timer);
}

void Model::initialize()
{
    std::ifstream file(kPath, std::ios::binary);
    if (!file || !m_keyed)
    {
        m_log.log<logging::LogLevel::Warning>("model disabled: file {} present = {}, NLX_MODEL_KEY valid = {}", std::string_view(kPath), static_cast<bool>(file), m_keyed);
        m_log.flush();

        m_initialized.store(true, std::memory_order_release);
        return;
    }

    const std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    std::vector<std::byte> sealed(raw.size());
    std::memcpy(sealed.data(), raw.data(), raw.size());

    std::vector<std::byte> plain;
    if (!crypto::open(sealed, m_key, plain))
    {
        throw std::runtime_error("model decryption failed: wrong NLX_MODEL_KEY or damaged model file");
    }

    std::string_view error;
    if (!m_network.load(plain, error))
    {
        throw std::runtime_error(std::format("model load failed: {}", error));
    }

    std::memset(plain.data(), 0, plain.size());

    m_ready = true;

    m_log.log("model loaded: parameters = {}, input = {}, state = {}, output = {}, vectorized = {}",
              m_network.parameters(), m_network.input_size(), m_network.state_size(), m_network.output_size(), m_network.vectorized());
    m_log.flush();

    m_initialized.store(true, std::memory_order_release);
}

void Model::start() noexcept
{
    if (!m_initialized.load(std::memory_order_acquire)) [[unlikely]]
    {
        NLX_FATAL("nlx::service::Model::start() called before initialize()");
    }

    m_log.log("model started");
    m_log.flush();

    m_loop.reset(time::now_monotonic());

    epoll_event event{};
    while (true)
    {
        const size_t count = detail::epoll_wait(m_epoll, { &event, 1 }, -1);
        m_loop.wake(time::now_monotonic());

        if (count != 0 && event.data.fd == m_request)
        {
            [[maybe_unused]] const uint64_t result = detail::drain(m_request);
            serve();
        }
        else if (count != 0 && event.data.fd == m_timer)
        {
            [[maybe_unused]] const uint64_t ticks = detail::drain(m_timer);
            report();
        }
        else if (count != 0 && event.data.fd == m_stop)
        {
            [[maybe_unused]] const uint64_t result = detail::drain(m_stop);
            break;
        }

        m_loop.sleep(time::now_monotonic());
    }

    report();

    m_log.log("model stopped");
    m_log.flush();
}

void Model::request_stop() noexcept
{
    detail::notify(m_stop);
}

bool Model::submit(const ModelRequest &request) noexcept
{
    if (!m_ready || !m_requests.push(request)) [[unlikely]]
    {
        return false;
    }

    detail::notify(m_request);

    return true;
}

bool Model::ready() const noexcept
{
    return m_ready;
}

size_t Model::input_size() const noexcept
{
    return m_network.input_size();
}

size_t Model::state_size() const noexcept
{
    return m_network.state_size();
}

size_t Model::output_size() const noexcept
{
    return m_network.output_size();
}

void Model::serve() noexcept
{
    ModelRequest request{};
    while (m_requests.pop(request))
    {
        const nanoseconds begin = time::now_monotonic();

        m_network.step({ request.input, m_network.input_size() }, { request.state, m_network.state_size() }, { request.output, m_network.output_size() });

        const nanoseconds end = time::now_monotonic();

        m_waiting.record(begin - request.submitted);
        m_inference.record(end - begin);

        detail::notify(*request.notify);
    }
}

void Model::report() noexcept
{
    const nanoseconds now = time::now_monotonic();

    m_log.log<logging::LogLevel::Metric>("model metrics: utilization = {} %, steps = {}, inference p50 = {} us, p99 = {} us, max = {} us, queue p99 = {} us",
              m_loop.utilization(now), m_inference.count(), m_inference.percentile(0.5), m_inference.percentile(0.99), m_inference.max(), m_waiting.percentile(0.99));
    m_log.flush();

    m_loop.reset(now);
    m_inference.reset();
    m_waiting.reset();
}

} // namespace nlx::service
