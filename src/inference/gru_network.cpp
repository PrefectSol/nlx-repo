#include "inference/gru_network.h"

#include <immintrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace nlx::inference
{

namespace
{

constexpr size_t kLanes = 8;
constexpr size_t kAlignment = 64;
constexpr size_t kRegion = kAlignment / sizeof(float);

constexpr uint32_t kIdentity = 0;
constexpr uint32_t kRelu = 1;
constexpr uint32_t kTanh = 2;
constexpr uint32_t kSigmoid = 3;

constexpr size_t pad(size_t value, size_t step) noexcept
{
    return (value + step - 1) / step * step;
}

struct Reader
{
    const std::byte *data;
    size_t size;
    size_t offset;

    bool word(uint32_t &value) noexcept
    {
        if (offset + sizeof(value) > size)
        {
            return false;
        }

        std::memcpy(&value, data + offset, sizeof(value));
        offset += sizeof(value);

        return true;
    }

    float number(size_t index) const noexcept
    {
        float value = 0.0f;
        std::memcpy(&value, data + offset + index * sizeof(float), sizeof(float));

        return value;
    }
};

float sigmoid(float value) noexcept
{
    return 1.0f / (1.0f + std::exp(-value));
}

float activate(float value, uint32_t activation) noexcept
{
    if (activation == kRelu)
    {
        return std::max(value, 0.0f);
    }

    if (activation == kTanh)
    {
        return std::tanh(value);
    }

    if (activation == kSigmoid)
    {
        return sigmoid(value);
    }

    return value;
}

__attribute__((target("avx2,fma"))) inline __m256 exp8(__m256 value) noexcept
{
    const __m256 clamped = _mm256_max_ps(_mm256_min_ps(value, _mm256_set1_ps(88.3762626647949f)), _mm256_set1_ps(-88.3762626647949f));

    const __m256 scaled = _mm256_fmadd_ps(clamped, _mm256_set1_ps(1.44269504088896341f), _mm256_set1_ps(0.5f));
    const __m256 power = _mm256_floor_ps(scaled);

    __m256 reduced = _mm256_fnmadd_ps(power, _mm256_set1_ps(0.693359375f), clamped);
    reduced = _mm256_fnmadd_ps(power, _mm256_set1_ps(-2.12194440e-4f), reduced);

    __m256 polynomial = _mm256_set1_ps(1.9875691500e-4f);
    polynomial = _mm256_fmadd_ps(polynomial, reduced, _mm256_set1_ps(1.3981999507e-3f));
    polynomial = _mm256_fmadd_ps(polynomial, reduced, _mm256_set1_ps(8.3334519073e-3f));
    polynomial = _mm256_fmadd_ps(polynomial, reduced, _mm256_set1_ps(4.1665795894e-2f));
    polynomial = _mm256_fmadd_ps(polynomial, reduced, _mm256_set1_ps(1.6666665459e-1f));
    polynomial = _mm256_fmadd_ps(polynomial, reduced, _mm256_set1_ps(5.0000001201e-1f));

    const __m256 squared = _mm256_mul_ps(reduced, reduced);
    const __m256 result = _mm256_add_ps(_mm256_fmadd_ps(polynomial, squared, reduced), _mm256_set1_ps(1.0f));

    const __m256i exponent = _mm256_slli_epi32(_mm256_add_epi32(_mm256_cvttps_epi32(power), _mm256_set1_epi32(127)), 23);

    return _mm256_mul_ps(result, _mm256_castsi256_ps(exponent));
}

__attribute__((target("avx2,fma"))) inline __m256 sigmoid8(__m256 value) noexcept
{
    const __m256 one = _mm256_set1_ps(1.0f);
    const __m256 negative = _mm256_sub_ps(_mm256_setzero_ps(), value);

    return _mm256_div_ps(one, _mm256_add_ps(one, exp8(negative)));
}

__attribute__((target("avx2,fma"))) inline __m256 tanh8(__m256 value) noexcept
{
    const __m256 squashed = sigmoid8(_mm256_add_ps(value, value));
    return _mm256_sub_ps(_mm256_add_ps(squashed, squashed), _mm256_set1_ps(1.0f));
}

__attribute__((target("avx2,fma"))) inline __m256 activate8(__m256 value, uint32_t activation) noexcept
{
    if (activation == kRelu)
    {
        return _mm256_max_ps(value, _mm256_setzero_ps());
    }

    if (activation == kTanh)
    {
        return tanh8(value);
    }

    if (activation == kSigmoid)
    {
        return sigmoid8(value);
    }

    return value;
}

} // namespace

void GruNetwork::Release::operator()(float *data) const noexcept
{
    ::operator delete[](data, std::align_val_t{ kAlignment });
}

GruNetwork::GruNetwork() noexcept :
    m_storage(),
    m_layers(),
    m_dense(),
    m_layers_size(0),
    m_dense_size(0),
    m_input(0),
    m_hidden(0),
    m_output(0),
    m_parameters(0),
    m_joined(nullptr),
    m_gates(nullptr),
    m_candidate(nullptr),
    m_recurrent(nullptr),
    m_front(nullptr),
    m_back(nullptr),
    m_vectorized(false)
{

}

bool GruNetwork::load(std::span<const std::byte> model, std::string_view &error)
{
    Reader reader{ model.data(), model.size(), 0 };

    const bool magic = model.size() >= 4 && std::memcmp(model.data(), "NLXM", 4) == 0;
    if (!magic)
    {
        error = "not a model";
        return false;
    }

    reader.offset = 4;

    std::array<uint32_t, 5> header{};
    for (uint32_t &field : header)
    {
        if (!reader.word(field))
        {
            error = "truncated header";
            return false;
        }
    }

    const uint32_t version = header[0];
    const size_t input = header[1];
    const size_t hidden = header[2];
    const size_t layers = header[3];
    const size_t dense = header[4];

    bool valid = version == 1 && input != 0 && input <= kWidthCapacity && hidden != 0 && hidden <= kWidthCapacity;
    valid = valid && layers != 0 && layers <= kLayersCapacity && dense != 0 && dense <= kDenseCapacity;

    if (!valid)
    {
        error = "unsupported header";
        return false;
    }

    std::array<uint32_t, kDenseCapacity> outputs{};
    std::array<uint32_t, kDenseCapacity> activations{};

    for (size_t index = 0; index < dense; ++index)
    {
        const bool read = reader.word(outputs[index]);
        if (!read || outputs[index] == 0 || outputs[index] > kWidthCapacity)
        {
            error = "unsupported dense layer";
            return false;
        }
    }

    for (size_t index = 0; index < dense; ++index)
    {
        const bool read = reader.word(activations[index]);
        if (!read || activations[index] > kSigmoid)
        {
            error = "unsupported activation";
            return false;
        }
    }

    size_t parameters = 0;
    size_t storage = 0;
    size_t joined = 0;
    size_t width = hidden;

    size_t size = input;
    for (size_t layer = 0; layer < layers; ++layer)
    {
        parameters += 3 * hidden * size + 3 * hidden * hidden + 6 * hidden;

        storage += pad(pad(2 * hidden, kLanes) * (size + hidden) + pad(2 * hidden, kLanes), kRegion);
        storage += pad(pad(hidden, kLanes) * size + pad(hidden, kLanes), kRegion);
        storage += pad(pad(hidden, kLanes) * hidden + pad(hidden, kLanes), kRegion);

        joined = std::max(joined, size + hidden);
        size = hidden;
    }

    for (size_t index = 0; index < dense; ++index)
    {
        parameters += outputs[index] * size + outputs[index];
        storage += pad(pad(outputs[index], kLanes) * size + pad(outputs[index], kLanes), kRegion);

        width = std::max(width, size_t{ outputs[index] });
        size = outputs[index];
    }

    if (reader.offset + parameters * sizeof(float) != model.size())
    {
        error = "size does not match the header";
        return false;
    }

    const size_t scratch = pad(joined, kRegion) + pad(2 * hidden, kRegion) + 2 * pad(hidden, kRegion) + 2 * pad(pad(width, kLanes), kRegion);
    const size_t total = storage + scratch;

    float *const memory = static_cast<float *>(::operator new[](total * sizeof(float), std::align_val_t{ kAlignment }));
    m_storage.reset(memory);
    std::fill_n(memory, total, 0.0f);

    float *cursor = memory;
    size_t source = 0;

    size = input;
    for (size_t layer = 0; layer < layers; ++layer)
    {
        Layer &target = m_layers[layer];
        target.size = size;

        const size_t weight_ih = source;
        const size_t weight_hh = weight_ih + 3 * hidden * size;
        const size_t bias_ih = weight_hh + 3 * hidden * hidden;
        const size_t bias_hh = bias_ih + 3 * hidden;
        source = bias_hh + 3 * hidden;

        const size_t gates_rows = pad(2 * hidden, kLanes);
        const size_t gates_columns = size + hidden;

        float *panels = cursor;
        float *bias = panels + gates_rows * gates_columns;
        for (size_t row = 0; row < 2 * hidden; ++row)
        {
            float *const lane = panels + row / kLanes * gates_columns * kLanes + row % kLanes;
            for (size_t column = 0; column < size; ++column)
            {
                lane[column * kLanes] = reader.number(weight_ih + row * size + column);
            }

            for (size_t column = 0; column < hidden; ++column)
            {
                lane[(size + column) * kLanes] = reader.number(weight_hh + row * hidden + column);
            }

            bias[row] = reader.number(bias_ih + row) + reader.number(bias_hh + row);
        }

        target.gates = Matrix{ panels, bias, 2 * hidden, gates_rows / kLanes, gates_columns };
        cursor += pad(gates_rows * gates_columns + gates_rows, kRegion);

        const size_t rows = pad(hidden, kLanes);

        panels = cursor;
        bias = panels + rows * size;
        for (size_t row = 0; row < hidden; ++row)
        {
            float *const lane = panels + row / kLanes * size * kLanes + row % kLanes;
            for (size_t column = 0; column < size; ++column)
            {
                lane[column * kLanes] = reader.number(weight_ih + (2 * hidden + row) * size + column);
            }

            bias[row] = reader.number(bias_ih + 2 * hidden + row);
        }

        target.input = Matrix{ panels, bias, hidden, rows / kLanes, size };
        cursor += pad(rows * size + rows, kRegion);

        panels = cursor;
        bias = panels + rows * hidden;
        for (size_t row = 0; row < hidden; ++row)
        {
            float *const lane = panels + row / kLanes * hidden * kLanes + row % kLanes;
            for (size_t column = 0; column < hidden; ++column)
            {
                lane[column * kLanes] = reader.number(weight_hh + (2 * hidden + row) * hidden + column);
            }

            bias[row] = reader.number(bias_hh + 2 * hidden + row);
        }

        target.hidden = Matrix{ panels, bias, hidden, rows / kLanes, hidden };
        cursor += pad(rows * hidden + rows, kRegion);

        size = hidden;
    }

    for (size_t index = 0; index < dense; ++index)
    {
        const size_t outputs_size = outputs[index];
        const size_t rows = pad(outputs_size, kLanes);

        float *const panels = cursor;
        float *const bias = panels + rows * size;
        for (size_t row = 0; row < outputs_size; ++row)
        {
            float *const lane = panels + row / kLanes * size * kLanes + row % kLanes;
            for (size_t column = 0; column < size; ++column)
            {
                lane[column * kLanes] = reader.number(source + row * size + column);
            }
        }

        source += outputs_size * size;
        for (size_t row = 0; row < outputs_size; ++row)
        {
            bias[row] = reader.number(source + row);
        }

        source += outputs_size;

        m_dense[index] = Dense{ Matrix{ panels, bias, outputs_size, rows / kLanes, size }, activations[index] };
        cursor += pad(rows * size + rows, kRegion);

        size = outputs_size;
    }

    m_joined = cursor;
    cursor += pad(joined, kRegion);
    m_gates = cursor;
    cursor += pad(2 * hidden, kRegion);
    m_candidate = cursor;
    cursor += pad(hidden, kRegion);
    m_recurrent = cursor;
    cursor += pad(hidden, kRegion);
    m_front = cursor;
    cursor += pad(pad(width, kLanes), kRegion);
    m_back = cursor;

    m_layers_size = layers;
    m_dense_size = dense;
    m_input = input;
    m_hidden = hidden;
    m_output = outputs[dense - 1];
    m_parameters = parameters;
    m_vectorized = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");

    return true;
}

namespace
{

struct Kernel
{
    static void product(const float *panels, const float *bias, size_t blocks, size_t columns, const float *input, float *output) noexcept
    {
        for (size_t block = 0; block < blocks; ++block)
        {
            const float *const panel = panels + block * columns * kLanes;
            for (size_t lane = 0; lane < kLanes; ++lane)
            {
                float sum = bias[block * kLanes + lane];
                for (size_t column = 0; column < columns; ++column)
                {
                    sum += panel[column * kLanes + lane] * input[column];
                }

                output[block * kLanes + lane] = sum;
            }
        }
    }

    static void update(const float *gates, const float *candidate, const float *recurrent, float *state, size_t hidden) noexcept
    {
        for (size_t index = 0; index < hidden; ++index)
        {
            const float reset = sigmoid(gates[index]);
            const float keep = sigmoid(gates[hidden + index]);
            const float proposal = std::tanh(candidate[index] + reset * recurrent[index]);

            state[index] = proposal + keep * (state[index] - proposal);
        }
    }

    static void apply(float *values, size_t size, uint32_t activation) noexcept
    {
        for (size_t index = 0; index < size; ++index)
        {
            values[index] = activate(values[index], activation);
        }
    }
};

struct VectorKernel
{
    __attribute__((target("avx2,fma"))) static void product(const float *panels, const float *bias, size_t blocks, size_t columns,
                                                            const float *input, float *output) noexcept
    {
        const size_t stride = columns * kLanes;

        size_t block = 0;
        for (; block + 4 <= blocks; block += 4)
        {
            const float *const first = panels + block * stride;
            const float *const second = first + stride;
            const float *const third = second + stride;
            const float *const fourth = third + stride;

            __m256 a0 = _mm256_setzero_ps();
            __m256 a1 = _mm256_setzero_ps();
            __m256 a2 = _mm256_setzero_ps();
            __m256 a3 = _mm256_setzero_ps();
            __m256 b0 = _mm256_setzero_ps();
            __m256 b1 = _mm256_setzero_ps();
            __m256 b2 = _mm256_setzero_ps();
            __m256 b3 = _mm256_setzero_ps();

            size_t column = 0;
            for (; column + 2 <= columns; column += 2)
            {
                const size_t offset = column * kLanes;
                const __m256 x0 = _mm256_broadcast_ss(input + column);
                const __m256 x1 = _mm256_broadcast_ss(input + column + 1);

                a0 = _mm256_fmadd_ps(_mm256_load_ps(first + offset), x0, a0);
                a1 = _mm256_fmadd_ps(_mm256_load_ps(second + offset), x0, a1);
                a2 = _mm256_fmadd_ps(_mm256_load_ps(third + offset), x0, a2);
                a3 = _mm256_fmadd_ps(_mm256_load_ps(fourth + offset), x0, a3);

                b0 = _mm256_fmadd_ps(_mm256_load_ps(first + offset + kLanes), x1, b0);
                b1 = _mm256_fmadd_ps(_mm256_load_ps(second + offset + kLanes), x1, b1);
                b2 = _mm256_fmadd_ps(_mm256_load_ps(third + offset + kLanes), x1, b2);
                b3 = _mm256_fmadd_ps(_mm256_load_ps(fourth + offset + kLanes), x1, b3);
            }

            if (column < columns)
            {
                const size_t offset = column * kLanes;
                const __m256 x0 = _mm256_broadcast_ss(input + column);

                a0 = _mm256_fmadd_ps(_mm256_load_ps(first + offset), x0, a0);
                a1 = _mm256_fmadd_ps(_mm256_load_ps(second + offset), x0, a1);
                a2 = _mm256_fmadd_ps(_mm256_load_ps(third + offset), x0, a2);
                a3 = _mm256_fmadd_ps(_mm256_load_ps(fourth + offset), x0, a3);
            }

            const float *const shift = bias + block * kLanes;
            float *const target = output + block * kLanes;

            _mm256_store_ps(target, _mm256_add_ps(_mm256_add_ps(a0, b0), _mm256_load_ps(shift)));
            _mm256_store_ps(target + kLanes, _mm256_add_ps(_mm256_add_ps(a1, b1), _mm256_load_ps(shift + kLanes)));
            _mm256_store_ps(target + 2 * kLanes, _mm256_add_ps(_mm256_add_ps(a2, b2), _mm256_load_ps(shift + 2 * kLanes)));
            _mm256_store_ps(target + 3 * kLanes, _mm256_add_ps(_mm256_add_ps(a3, b3), _mm256_load_ps(shift + 3 * kLanes)));
        }

        for (; block < blocks; ++block)
        {
            const float *const panel = panels + block * stride;

            __m256 a0 = _mm256_setzero_ps();
            __m256 b0 = _mm256_setzero_ps();

            size_t column = 0;
            for (; column + 2 <= columns; column += 2)
            {
                a0 = _mm256_fmadd_ps(_mm256_load_ps(panel + column * kLanes), _mm256_broadcast_ss(input + column), a0);
                b0 = _mm256_fmadd_ps(_mm256_load_ps(panel + column * kLanes + kLanes), _mm256_broadcast_ss(input + column + 1), b0);
            }

            if (column < columns)
            {
                a0 = _mm256_fmadd_ps(_mm256_load_ps(panel + column * kLanes), _mm256_broadcast_ss(input + column), a0);
            }

            _mm256_store_ps(output + block * kLanes, _mm256_add_ps(_mm256_add_ps(a0, b0), _mm256_load_ps(bias + block * kLanes)));
        }
    }

    __attribute__((target("avx2,fma"))) static void update(const float *gates, const float *candidate, const float *recurrent, float *state,
                                                           size_t hidden) noexcept
    {
        size_t index = 0;
        for (; index + kLanes <= hidden; index += kLanes)
        {
            const __m256 reset = sigmoid8(_mm256_loadu_ps(gates + index));
            const __m256 keep = sigmoid8(_mm256_loadu_ps(gates + hidden + index));
            const __m256 proposal = tanh8(_mm256_fmadd_ps(reset, _mm256_loadu_ps(recurrent + index), _mm256_loadu_ps(candidate + index)));
            const __m256 previous = _mm256_loadu_ps(state + index);

            _mm256_storeu_ps(state + index, _mm256_fmadd_ps(keep, _mm256_sub_ps(previous, proposal), proposal));
        }

        Kernel::update(gates + index, candidate + index, recurrent + index, state + index, hidden - index);
    }

    __attribute__((target("avx2,fma"))) static void apply(float *values, size_t size, uint32_t activation) noexcept
    {
        if (activation == kIdentity)
        {
            return;
        }

        size_t index = 0;
        for (; index + kLanes <= size; index += kLanes)
        {
            _mm256_storeu_ps(values + index, activate8(_mm256_loadu_ps(values + index), activation));
        }

        Kernel::apply(values + index, size - index, activation);
    }
};

} // namespace

void GruNetwork::step(std::span<const float> input, std::span<float> state, std::span<float> output) noexcept
{
    const float *value = input.data();
    size_t size = m_input;

    for (size_t layer = 0; layer < m_layers_size; ++layer)
    {
        const Layer &weights = m_layers[layer];
        float *const hidden = state.data() + layer * m_hidden;

        std::copy_n(value, size, m_joined);
        std::copy_n(hidden, m_hidden, m_joined + size);

        if (m_vectorized)
        {
            VectorKernel::product(weights.gates.panels, weights.gates.bias, weights.gates.blocks, weights.gates.columns, m_joined, m_gates);
            VectorKernel::product(weights.input.panels, weights.input.bias, weights.input.blocks, weights.input.columns, m_joined, m_candidate);
            VectorKernel::product(weights.hidden.panels, weights.hidden.bias, weights.hidden.blocks, weights.hidden.columns, m_joined + size, m_recurrent);
            VectorKernel::update(m_gates, m_candidate, m_recurrent, hidden, m_hidden);
        }
        else
        {
            Kernel::product(weights.gates.panels, weights.gates.bias, weights.gates.blocks, weights.gates.columns, m_joined, m_gates);
            Kernel::product(weights.input.panels, weights.input.bias, weights.input.blocks, weights.input.columns, m_joined, m_candidate);
            Kernel::product(weights.hidden.panels, weights.hidden.bias, weights.hidden.blocks, weights.hidden.columns, m_joined + size, m_recurrent);
            Kernel::update(m_gates, m_candidate, m_recurrent, hidden, m_hidden);
        }

        value = hidden;
        size = m_hidden;
    }

    std::copy_n(value, size, m_front);

    float *front = m_front;
    float *back = m_back;

    for (size_t index = 0; index < m_dense_size; ++index)
    {
        const Dense &dense = m_dense[index];
        const Matrix &weights = dense.weights;

        if (m_vectorized)
        {
            VectorKernel::product(weights.panels, weights.bias, weights.blocks, weights.columns, front, back);
            VectorKernel::apply(back, weights.rows, dense.activation);
        }
        else
        {
            Kernel::product(weights.panels, weights.bias, weights.blocks, weights.columns, front, back);
            Kernel::apply(back, weights.rows, dense.activation);
        }

        std::swap(front, back);
    }

    std::copy_n(front, m_output, output.data());
}

size_t GruNetwork::input_size() const noexcept
{
    return m_input;
}

size_t GruNetwork::state_size() const noexcept
{
    return m_layers_size * m_hidden;
}

size_t GruNetwork::output_size() const noexcept
{
    return m_output;
}

size_t GruNetwork::parameters() const noexcept
{
    return m_parameters;
}

bool GruNetwork::vectorized() const noexcept
{
    return m_vectorized;
}

} // namespace nlx::inference
