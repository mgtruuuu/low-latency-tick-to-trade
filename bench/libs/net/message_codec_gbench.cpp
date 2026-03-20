/**
 * @file message_codec_gbench.cpp
 * @brief Google Benchmark version of message codec benchmarks.
 *
 * Companion to message_codec_bench.cpp (custom rdtsc). This version uses
 * Google Benchmark for CI-friendly output, regression tracking, and JSON
 * export.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/net/message_codec_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/net/message_codec_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/net/message_codec_gbench
 */

#include "net/message_codec.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>

using namespace mk::net;

namespace {

constexpr std::size_t kPayloadSize = 64;
constexpr std::size_t kBufSize = kMessageHeaderSize + kPayloadSize + 16;

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_PackMessage(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize]{};
  alignas(64) std::byte payload[kPayloadSize]{};
  for (std::size_t i = 0; i < kPayloadSize; ++i) {
    payload[i] = static_cast<std::byte>(i & 0xFF);
  }

  for (auto _ : state) {
    auto n = pack_message(std::span{buf}, 1, 42, 0,
                          std::span<const std::byte>{payload});
    benchmark::DoNotOptimize(n);
  }
}
BENCHMARK(BM_PackMessage);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_UnpackMessage(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize]{};
  alignas(64) std::byte payload[kPayloadSize]{};
  for (std::size_t i = 0; i < kPayloadSize; ++i) {
    payload[i] = static_cast<std::byte>(i & 0xFF);
  }
  (void)pack_message(std::span{buf}, 1, 42, 0,
                     std::span<const std::byte>{payload});

  ParsedMessageView view{};
  for (auto _ : state) {
    auto ok =
        unpack_message(std::span<const std::byte>{buf, kBufSize}, view);
    benchmark::DoNotOptimize(ok);
    benchmark::DoNotOptimize(view);
  }
}
BENCHMARK(BM_UnpackMessage);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_MessageRoundtrip(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize]{};
  alignas(64) std::byte payload[kPayloadSize]{};
  for (std::size_t i = 0; i < kPayloadSize; ++i) {
    payload[i] = static_cast<std::byte>(i & 0xFF);
  }

  for (auto _ : state) {
    (void)pack_message(std::span{buf}, 1, 42, 0,
                       std::span<const std::byte>{payload});
    ParsedMessageView view{};
    (void)unpack_message(std::span<const std::byte>{buf, kBufSize}, view);
    benchmark::DoNotOptimize(view);
  }
}
BENCHMARK(BM_MessageRoundtrip);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
