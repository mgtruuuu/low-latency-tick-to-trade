/**
 * @file length_prefix_codec_bench.cpp
 * @brief Microbenchmark for length-prefix frame codec operations.
 *
 * Measures the cost of encode_length_prefix_frame() and
 * decode_length_prefix_frame() for the wire format:
 *   [uint32_t total_len (NBO)][payload]
 *
 * Operations benchmarked:
 *   - encode_length_prefix_frame()  — write header + payload
 *   - decode_length_prefix_frame()  — parse header, zero-copy payload view
 *   - round-trip                    — encode + decode
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/net/length_prefix_codec_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/net/length_prefix_codec_bench
 */

#include "bench_utils.hpp"
#include "net/length_prefix_codec.hpp"
#include "sys/nano_clock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

using namespace mk::sys;
using namespace mk::net;
using namespace mk::bench;

namespace {

// ============================================================================
// Configuration
// ============================================================================

constexpr std::size_t kN = 10'000;
constexpr std::size_t kPayloadSize = 64;
constexpr std::size_t kBufSize = kLengthPrefixSize + kPayloadSize + 16;

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// Benchmarks
// ============================================================================

void bench_encode(const TscCalibration &cal) {
  alignas(64) std::byte buf[kBufSize]{};
  alignas(64) std::byte payload[kPayloadSize]{};
  for (std::size_t i = 0; i < kPayloadSize; ++i) {
    payload[i] = static_cast<std::byte>(i & 0xFF);
  }

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    do_not_optimize(encode_length_prefix_frame(
        std::span{buf}, std::span<const std::byte>{payload}));
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    auto n = encode_length_prefix_frame(std::span{buf},
                                        std::span<const std::byte>{payload});
    const auto t1 = rdtsc_end();
    do_not_optimize(n);
    g_latencies[i] = t1 - t0;
  }
  print_stats("encode()", compute_stats(cal, g_latencies));
}

void bench_decode(const TscCalibration &cal) {
  alignas(64) std::byte buf[kBufSize]{};
  alignas(64) std::byte payload[kPayloadSize]{};
  for (std::size_t i = 0; i < kPayloadSize; ++i) {
    payload[i] = static_cast<std::byte>(i & 0xFF);
  }
  (void)encode_length_prefix_frame(std::span{buf},
                                   std::span<const std::byte>{payload});

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    auto result =
        decode_length_prefix_frame(std::span<const std::byte>{buf, kBufSize});
    do_not_optimize(result);
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    auto result =
        decode_length_prefix_frame(std::span<const std::byte>{buf, kBufSize});
    const auto t1 = rdtsc_end();
    do_not_optimize(result);
    g_latencies[i] = t1 - t0;
  }
  print_stats("decode()", compute_stats(cal, g_latencies));
}

void bench_roundtrip(const TscCalibration &cal) {
  alignas(64) std::byte buf[kBufSize]{};
  alignas(64) std::byte payload[kPayloadSize]{};
  for (std::size_t i = 0; i < kPayloadSize; ++i) {
    payload[i] = static_cast<std::byte>(i & 0xFF);
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    (void)encode_length_prefix_frame(std::span{buf},
                                     std::span<const std::byte>{payload});
    auto result =
        decode_length_prefix_frame(std::span<const std::byte>{buf, kBufSize});
    const auto t1 = rdtsc_end();
    do_not_optimize(result);
    g_latencies[i] = t1 - t0;
  }
  print_stats("round-trip", compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== LengthPrefixCodec Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n", kN);
  std::printf("Payload size: %zu bytes  Header: %zu bytes\n\n", kPayloadSize,
              kLengthPrefixSize);

  print_header();
  bench_encode(cal);
  bench_decode(cal);
  bench_roundtrip(cal);

  std::printf(
      "\nTip: taskset -c N ./length_prefix_codec_bench  for stable p99.\n");

  return 0;
}
