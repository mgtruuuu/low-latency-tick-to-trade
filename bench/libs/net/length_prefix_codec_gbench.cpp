/**
 * @file length_prefix_codec_gbench.cpp
 * @brief Google Benchmark version of length-prefix codec benchmarks.
 *
 * Companion to length_prefix_codec_bench.cpp (custom rdtsc). This version
 * uses Google Benchmark for CI-friendly output, regression tracking, and
 * JSON export.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/net/length_prefix_codec_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/net/length_prefix_codec_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/net/length_prefix_codec_gbench
 */

#include "net/length_prefix_codec.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>

using namespace mk::net;

namespace {

constexpr std::size_t kPayloadSize = 64;
constexpr std::size_t kBufSize = kLengthPrefixSize + kPayloadSize + 16;

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Encode(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize]{};
  alignas(64) std::byte payload[kPayloadSize]{};
  for (std::size_t i = 0; i < kPayloadSize; ++i) {
    payload[i] = static_cast<std::byte>(i & 0xFF);
  }

  for (auto _ : state) {
    auto n = encode_length_prefix_frame(std::span{buf},
                                        std::span<const std::byte>{payload});
    benchmark::DoNotOptimize(n);
  }
}
BENCHMARK(BM_Encode);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Decode(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize]{};
  alignas(64) std::byte payload[kPayloadSize]{};
  for (std::size_t i = 0; i < kPayloadSize; ++i) {
    payload[i] = static_cast<std::byte>(i & 0xFF);
  }
  (void)encode_length_prefix_frame(std::span{buf},
                                   std::span<const std::byte>{payload});

  for (auto _ : state) {
    auto result =
        decode_length_prefix_frame(std::span<const std::byte>{buf, kBufSize});
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_Decode);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_LengthPrefixRoundtrip(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize]{};
  alignas(64) std::byte payload[kPayloadSize]{};
  for (std::size_t i = 0; i < kPayloadSize; ++i) {
    payload[i] = static_cast<std::byte>(i & 0xFF);
  }

  for (auto _ : state) {
    (void)encode_length_prefix_frame(std::span{buf},
                                     std::span<const std::byte>{payload});
    auto result =
        decode_length_prefix_frame(std::span<const std::byte>{buf, kBufSize});
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_LengthPrefixRoundtrip);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
