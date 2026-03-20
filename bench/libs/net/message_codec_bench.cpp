/**
 * @file message_codec_bench.cpp
 * @brief Microbenchmark for TLV-style message codec operations.
 *
 * Measures the cost of pack_message() and unpack_message() for the
 * TLV wire format: [magic:4][version:2][msg_type:2][payload_len:4][flags:4][payload:N]
 *
 * Operations benchmarked:
 *   - pack_message()     — serialize header + payload
 *   - unpack_message()   — parse header + zero-copy payload view
 *   - round-trip         — pack + unpack
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/net/message_codec_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/net/message_codec_bench
 */

#include "bench_utils.hpp"
#include "net/message_codec.hpp"
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
constexpr std::size_t kBufSize = kMessageHeaderSize + kPayloadSize + 16;

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// Benchmarks
// ============================================================================

void bench_pack_message(const TscCalibration &cal) {
  alignas(64) std::byte buf[kBufSize]{};
  alignas(64) std::byte payload[kPayloadSize]{};
  // Fill payload with non-zero data.
  for (std::size_t i = 0; i < kPayloadSize; ++i) {
    payload[i] = static_cast<std::byte>(i & 0xFF);
  }

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    do_not_optimize(pack_message(std::span{buf}, 1, 42, 0,
                                 std::span<const std::byte>{payload}));
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    auto n = pack_message(std::span{buf}, 1, 42, 0,
                          std::span<const std::byte>{payload});
    const auto t1 = rdtsc_end();
    do_not_optimize(n);
    g_latencies[i] = t1 - t0;
  }
  print_stats("pack_message()", compute_stats(cal, g_latencies));
}

void bench_unpack_message(const TscCalibration &cal) {
  alignas(64) std::byte buf[kBufSize]{};
  alignas(64) std::byte payload[kPayloadSize]{};
  for (std::size_t i = 0; i < kPayloadSize; ++i) {
    payload[i] = static_cast<std::byte>(i & 0xFF);
  }
  auto total = pack_message(std::span{buf}, 1, 42, 0,
                            std::span<const std::byte>{payload});
  do_not_optimize(total);

  // Warm-up.
  ParsedMessageView view{};
  for (int i = 0; i < 500; ++i) {
    do_not_optimize(
        unpack_message(std::span<const std::byte>{buf, kBufSize}, view));
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    auto ok = unpack_message(std::span<const std::byte>{buf, kBufSize}, view);
    const auto t1 = rdtsc_end();
    do_not_optimize(ok);
    do_not_optimize(view);
    g_latencies[i] = t1 - t0;
  }
  print_stats("unpack_message()", compute_stats(cal, g_latencies));
}

void bench_roundtrip(const TscCalibration &cal) {
  alignas(64) std::byte buf[kBufSize]{};
  alignas(64) std::byte payload[kPayloadSize]{};
  for (std::size_t i = 0; i < kPayloadSize; ++i) {
    payload[i] = static_cast<std::byte>(i & 0xFF);
  }

  for (std::size_t i = 0; i < kN; ++i) {
    ParsedMessageView view{};
    const auto t0 = rdtsc_start();
    (void)pack_message(std::span{buf}, 1, 42, 0,
                       std::span<const std::byte>{payload});
    (void)unpack_message(std::span<const std::byte>{buf, kBufSize}, view);
    const auto t1 = rdtsc_end();
    do_not_optimize(view);
    g_latencies[i] = t1 - t0;
  }
  print_stats("round-trip", compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== MessageCodec Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n", kN);
  std::printf("Payload size: %zu bytes  Header: %zu bytes\n\n", kPayloadSize,
              kMessageHeaderSize);

  print_header();
  bench_pack_message(cal);
  bench_unpack_message(cal);
  bench_roundtrip(cal);

  std::printf("\nTip: taskset -c N ./message_codec_bench  for stable p99.\n");

  return 0;
}
