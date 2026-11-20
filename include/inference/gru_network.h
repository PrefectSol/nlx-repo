#ifndef NLX_INFERENCE_GRU_NETWORK_H
#define NLX_INFERENCE_GRU_NETWORK_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace nlx::inference
{

class GruNetwork
{
public:
    static constexpr size_t kLayersCapacity = 4;
    static constexpr size_t kDenseCapacity = 8;
    static constexpr size_t kWidthCapacity = 4096;

    GruNetwork() noexcept;

    GruNetwork(const GruNetwork &) = delete;
    GruNetwork &operator=(const GruNetwork &) = delete;
    GruNetwork(GruNetwork &&) = delete;
    GruNetwork &operator=(GruNetwork &&) = delete;

    [[nodiscard]] bool load(std::span<const std::byte> model, std::string_view &error);

    void step(std::span<const float> input, std::span<float> state, std::span<float> output) noexcept;

    [[nodiscard]] size_t input_size() const noexcept;
    [[nodiscard]] size_t state_size() const noexcept;
    [[nodiscard]] size_t output_size() const noexcept;
    [[nodiscard]] size_t parameters() const noexcept;
    [[nodiscard]] bool vectorized() const noexcept;

private:
    struct Matrix
    {
        const float *panels;
        const float *bias;
        size_t rows;
        size_t blocks;
        size_t columns;
    };

    struct Layer
    {
        Matrix gates;
        Matrix input;
        Matrix hidden;
        size_t size;
    };

    struct Dense
    {
        Matrix weights;
        uint32_t activation;
    };

    struct Release
    {
        void operator()(float *data) const noexcept;
    };

    std::unique_ptr<float[], Release> m_storage;

    std::array<Layer, kLayersCapacity> m_layers;
    std::array<Dense, kDenseCapacity> m_dense;

    size_t m_layers_size;
    size_t m_dense_size;

    size_t m_input;
    size_t m_hidden;
    size_t m_output;
    size_t m_parameters;

    float *m_joined;
    float *m_gates;
    float *m_candidate;
    float *m_recurrent;
    float *m_front;
    float *m_back;

    bool m_vectorized;
};

} // namespace nlx::inference
#endif // !NLX_INFERENCE_GRU_NETWORK_H
