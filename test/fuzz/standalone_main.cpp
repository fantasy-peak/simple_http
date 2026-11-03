// Deterministic mutation driver for the fuzz targets when the project is
// built with GCC (no libFuzzer runtime).
//
// Each fuzz_*.cpp defines `LLVMFuzzerTestOneInput`, so the same source can be
// linked with libFuzzer under clang. This file supplies an equivalent bounded
// driver: load the seed corpus, then run deterministic mutations for a fixed
// number of iterations. It is not coverage-guided, but it exercises the same
// parsers under ASan/UBSan and keeps the targets useful on the default GCC
// toolchain.
//
//   fuzz_h1 --corpus test/fuzz/corpus/h1 --iterations 20000

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size);

namespace {

constexpr std::size_t kMaxInput = 64 * 1024;

std::vector<std::vector<std::uint8_t>> load_corpus(const std::filesystem::path &directory) {
    std::vector<std::vector<std::uint8_t>> seeds;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return seeds;
    }
    for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::ifstream input{entry.path(), std::ios::binary};
        if (!input) {
            continue;
        }
        std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        if (bytes.size() <= kMaxInput) {
            seeds.push_back(std::move(bytes));
        }
    }
    return seeds;
}

std::vector<std::vector<std::uint8_t>> builtin_seeds() {
    const std::string request = "GET / HTTP/1.1\r\nHost: example\r\n\r\n";
    const std::string chunked = "POST / HTTP/1.1\r\nHost: example\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
    const std::vector<std::uint8_t> h2_settings{0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
    const std::vector<std::uint8_t> h2_headers{0x00, 0x00, 0x05, 0x01, 0x05, 0x00, 0x00,
                                               0x00, 0x01, 0x82, 0x84, 0x86, 0x41, 0x8c};
    // A masked text frame "hello" (client-to-server shape).
    const std::vector<std::uint8_t> ws_masked{0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};
    const std::string multipart = "BOUNDARY\n--BOUNDARY\r\n"
                                  "Content-Disposition: form-data; name=\"a\"\r\n\r\n"
                                  "x\r\n--BOUNDARY--\r\n";
    return {
        {},        {request.begin(), request.end()},     {chunked.begin(), chunked.end()}, h2_settings, h2_headers,
        ws_masked, {multipart.begin(), multipart.end()},
    };
}

std::vector<std::uint8_t> mutate(std::mt19937_64 &rng, const std::vector<std::uint8_t> &seed) {
    std::vector<std::uint8_t> out = seed;
    const int rounds = 1 + static_cast<int>(rng() % 8);
    for (int round = 0; round < rounds; ++round) {
        switch (rng() % 5) {
        case 0:
            if (!out.empty()) {
                out[rng() % out.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
            }
            break;
        case 1:
            if (!out.empty()) {
                out[rng() % out.size()] = static_cast<std::uint8_t>(rng());
            }
            break;
        case 2: {
            const std::size_t count = 1 + rng() % 16;
            const std::size_t at = out.empty() ? 0 : rng() % (out.size() + 1);
            std::vector<std::uint8_t> block(count);
            for (auto &byte : block) {
                byte = static_cast<std::uint8_t>(rng());
            }
            out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), block.begin(), block.end());
            break;
        }
        case 3:
            if (!out.empty()) {
                const std::size_t at = rng() % out.size();
                const std::size_t count = std::min<std::size_t>(1 + rng() % 32, out.size() - at);
                out.erase(out.begin() + static_cast<std::ptrdiff_t>(at),
                          out.begin() + static_cast<std::ptrdiff_t>(at + count));
            }
            break;
        case 4:
            if (!out.empty()) {
                const std::size_t from = rng() % out.size();
                const std::size_t count = std::min<std::size_t>(1 + rng() % 32, out.size() - from);
                const std::size_t at = rng() % (out.size() + 1);
                std::vector<std::uint8_t> block(out.begin() + static_cast<std::ptrdiff_t>(from),
                                                out.begin() + static_cast<std::ptrdiff_t>(from + count));
                out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), block.begin(), block.end());
            }
            break;
        }
        if (out.size() > kMaxInput) {
            out.resize(kMaxInput);
        }
    }
    return out;
}

} // namespace

int main(int argc, char **argv) {
    std::filesystem::path corpus;
    std::size_t iterations = 20000;
    std::uint64_t seed_value = 0x5eed1234u;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--corpus" && i + 1 < argc) {
            corpus = argv[++i];
        } else if (arg == "--iterations" && i + 1 < argc) {
            iterations = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--seed" && i + 1 < argc) {
            seed_value = std::strtoull(argv[++i], nullptr, 10);
        }
    }

    auto seeds = load_corpus(corpus);
    if (seeds.empty()) {
        seeds = builtin_seeds();
    }

    // Run every seed verbatim first: a crash on a known-good shape is a real
    // regression, not a mutation artefact.
    for (const auto &seed : seeds) {
        LLVMFuzzerTestOneInput(seed.data(), seed.size());
    }

    std::mt19937_64 rng{seed_value};
    for (std::size_t i = 0; i < iterations; ++i) {
        std::vector<std::uint8_t> input;
        if (i % 16 == 0) {
            const std::size_t size = static_cast<std::size_t>(rng() % 4096);
            input.resize(size);
            for (auto &byte : input) {
                byte = static_cast<std::uint8_t>(rng());
            }
        } else {
            input = mutate(rng, seeds[static_cast<std::size_t>(rng() % seeds.size())]);
        }
        LLVMFuzzerTestOneInput(input.data(), input.size());
        if ((i + 1) % 5000 == 0) {
            std::cerr << "  " << (i + 1) << "/" << iterations << " iterations\n";
        }
    }
    std::cout << "fuzz: " << iterations << " iterations, " << seeds.size() << " seeds\n";
    return 0;
}
