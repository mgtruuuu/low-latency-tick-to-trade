/**
 * @file endian_bench.cpp
 * @brief Microbenchmark for endian conversion and load/store operations.
 *
 * Operations benchmarked:
 *   - host_to_be64()  — byte swap (BSWAP on x86 LE host)
 *   - be64_to_host()  — byte swap (self-inverse)
 *   - load_be64()     — memcpy + endian conversion from byte buffer
 *   - store_be64()    — endian conversion + memcpy to byte buffer
 *
 * These should be near-zero cost on x86-64 (single BSWAP instruction for
 * conversion, memcpy optimized away by compiler).
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/endian_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/endian_bench
 */

#include "bench_utils.hpp"
#include "sys/endian.hpp"
#include "sys/nano_clock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

using namespace mk::sys;
using namespace mk::bench;

namespace {

// ============================================================================
// Configuration
// ============================================================================

constexpr std::size_t kN = 10'000;

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// Benchmarks
// ============================================================================

void bench_host_to_be64(const TscCalibration &cal) {
  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    do_not_optimize(host_to_be64(static_cast<std::uint64_t>(i)));
  }

  for (std::size_t i = 0; i < kN; ++i) {
    // Use varying input to prevent constant folding.
    const auto input = static_cast<std::uint64_t>(i | 1);
    const auto t0 = rdtsc_start();
    auto result = host_to_be64(input);
    const auto t1 = rdtsc_end();
    do_not_optimize(result);
    g_latencies[i] = t1 - t0;
  }
  print_stats("host_to_be64()", compute_stats(cal, g_latencies));
}

void bench_be64_to_host(const TscCalibration &cal) {
  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    do_not_optimize(be64_to_host(static_cast<std::uint64_t>(i)));
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto input = static_cast<std::uint64_t>(i | 1);
    const auto t0 = rdtsc_start();
    auto result = be64_to_host(input);
    const auto t1 = rdtsc_end();
    do_not_optimize(result);
    g_latencies[i] = t1 - t0;
  }
  print_stats("be64_to_host()", compute_stats(cal, g_latencies));
}

void bench_load_be64(const TscCalibration &cal) {
  alignas(64) std::byte buf[8]{};
  // Pre-store a value so the load has something to read.
  store_be64(buf, 0xDEADBEEFCAFEBABEULL);

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    do_not_optimize(load_be64(buf));
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    auto result = load_be64(buf);
    const auto t1 = rdtsc_end();
    do_not_optimize(result);
    g_latencies[i] = t1 - t0;
  }
  print_stats("load_be64()", compute_stats(cal, g_latencies));
}

void bench_store_be64(const TscCalibration &cal) {
  alignas(64) std::byte buf[8]{};

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    store_be64(buf, static_cast<std::uint64_t>(i));
    do_not_optimize(buf);
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto val = static_cast<std::uint64_t>(i | 1);
    const auto t0 = rdtsc_start();
    store_be64(buf, val);
    const auto t1 = rdtsc_end();
    do_not_optimize(buf);
    g_latencies[i] = t1 - t0;
  }
  print_stats("store_be64()", compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== Endian Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n\n", kN);

  print_header();
  bench_host_to_be64(cal);
  bench_be64_to_host(cal);
  bench_load_be64(cal);
  bench_store_be64(cal);

  std::printf("\nNote: On x86-64 (LE host), BE conversion = single BSWAP.\n");
  std::printf("Tip:  taskset -c N ./endian_bench  for stable p99.\n");

  return 0;
}
