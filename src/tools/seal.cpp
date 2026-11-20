#include "core/crypto.h"
#include "inference/gru_network.h"
#include "parsing/env_reader.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

std::vector<std::byte> read(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error(std::string("cannot read ") + path);
    }

    const std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    std::vector<std::byte> bytes(raw.size());
    std::memcpy(bytes.data(), raw.data(), raw.size());

    return bytes;
}

nlx::crypto::AesKey key(const char *env)
{
    const nlx::parsing::EnvReader reader(env);

    nlx::crypto::AesKey result{};
    if (!nlx::crypto::parse_key(reader.get("NLX_MODEL_KEY"), result))
    {
        throw std::runtime_error("NLX_MODEL_KEY must be 64 hex characters");
    }

    return result;
}

void describe(std::span<const std::byte> model)
{
    nlx::inference::GruNetwork network;
    std::string_view error;

    if (!network.load(model, error))
    {
        throw std::runtime_error(std::string("not a valid model: ") + std::string(error));
    }

    std::printf("model: parameters = %zu, input = %zu, state = %zu, output = %zu\n", network.parameters(), network.input_size(), network.state_size(),
                network.output_size());
}

int usage()
{
    std::printf("usage:\n"
                "  nlx_seal key                                  print a new random NLX_MODEL_KEY\n"
                "  nlx_seal seal <model.nlxm> <model.nlxe> [env]  encrypt a model with NLX_MODEL_KEY from env (default .env)\n"
                "  nlx_seal check <model.nlxe> [env]             decrypt in memory and validate\n");
    return 1;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        return usage();
    }

    const std::string_view command(argv[1]);

    try
    {
        if (command == "key")
        {
            nlx::crypto::AesKey generated{};
            if (!nlx::crypto::random_bytes(generated))
            {
                throw std::runtime_error("random generator failed");
            }

            for (const uint8_t byte : generated)
            {
                std::printf("%02x", byte);
            }

            std::printf("\n");
            return 0;
        }

        if (command == "seal" && argc >= 4)
        {
            const std::vector<std::byte> plain = read(argv[2]);
            describe(plain);

            std::vector<std::byte> sealed;
            if (!nlx::crypto::seal(plain, key(argc >= 5 ? argv[4] : ".env"), sealed))
            {
                throw std::runtime_error("encryption failed");
            }

            std::ofstream file(argv[3], std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char *>(sealed.data()), static_cast<std::streamsize>(sealed.size()));
            if (!file)
            {
                throw std::runtime_error(std::string("cannot write ") + argv[3]);
            }

            std::printf("sealed %zu bytes into %s\n", sealed.size(), argv[3]);
            return 0;
        }

        if (command == "check" && argc >= 3)
        {
            std::vector<std::byte> plain;
            if (!nlx::crypto::open(read(argv[2]), key(argc >= 4 ? argv[3] : ".env"), plain))
            {
                throw std::runtime_error("decryption failed: wrong key or damaged file");
            }

            describe(plain);
            return 0;
        }
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "nlx_seal: %s\n", error.what());
        return 1;
    }

    return usage();
}
