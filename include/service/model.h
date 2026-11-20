#ifndef NLX_SERVICE_MODEL_H
#define NLX_SERVICE_MODEL_H

#include "core/credentials.h"
#include "core/crypto.h"
#include "core/file_descriptor.h"
#include "core/metrics.h"
#include "core/spsc_queue.h"
#include "core/time.h"
#include "inference/gru_network.h"
#include "logging/logger_node.h"
#include "logging/logger_registry.h"

#include <atomic>
#include <cstddef>

namespace nlx::service
{

struct ModelRequest
{
    const float *input;
    float *state;
    float *output;
    const FileDescriptor *notify;
    nanoseconds submitted;
};

class Model
{
public:
    explicit Model(const Credentials &credentials, logging::LoggerRegistry &registry);

    Model(const Model &) = delete;
    Model &operator=(const Model &) = delete;
    Model(Model &&) = delete;
    Model &operator=(Model &&) = delete;

    void initialize();
    void start() noexcept;
    void request_stop() noexcept;

    [[nodiscard]] bool submit(const ModelRequest &request) noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] size_t input_size() const noexcept;
    [[nodiscard]] size_t state_size() const noexcept;
    [[nodiscard]] size_t output_size() const noexcept;

private:
    static constexpr char kPath[] = "models/model.nlxe";

    static constexpr size_t kRequestsCapacity = 16;
    static constexpr size_t kMetricsMilliseconds = 60000;

    logging::LoggerNode m_log;
    inference::GruNetwork m_network;

    crypto::AesKey m_key;
    bool m_keyed;

    FileDescriptor m_epoll;
    FileDescriptor m_stop;
    FileDescriptor m_request;
    FileDescriptor m_timer;

    std::atomic<bool> m_initialized;
    bool m_ready;

    SpscQueue<ModelRequest, kRequestsCapacity> m_requests;

    LoopMeter m_loop;
    LatencyHistogram m_inference;
    LatencyHistogram m_waiting;

    void serve() noexcept;
    void report() noexcept;
};

} // namespace nlx::service
#endif // !NLX_SERVICE_MODEL_H
