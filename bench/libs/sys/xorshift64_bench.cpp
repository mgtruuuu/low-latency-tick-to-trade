/**
 * @file xorshift64_bench.cpp
 * @brief Microbenchmark for Xorshift64 PRNG throughput.
 *
 * Measures the cost of a single xorshift64 call (3 XOR-shift operations).
 * Expected to be sub-nanosecond on modern x86-64.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/xorshift64_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/xorshift64_bench
 */

#include "bench_utils.hpp"
#include "sys/nano_clock.hpp"
#include "sys/xorshift64.hpp"

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

void bench_xorshift64(const TscCalibration &cal) {
  Xorshift64 rng(0xDEAD'BEEF'CAFE'1234ULL);

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    do_not_optimize(rng());
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    auto val = rng();
    const auto t1 = rdtsc_end();
    do_not_optimize(val);
    g_latencies[i] = t1 - t0;
  }
  print_stats("operator()", compute_stats(cal, g_latencies));
}

/// Measure 8 consecutive calls to capture throughput with ILP.
void bench_xorshift64_batch8(const TscCalibration &cal) {
  Xorshift64 rng(0xDEAD'BEEF'CAFE'5678ULL);

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    do_not_optimize(rng());
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    auto v0 = rng();
    auto v1 = rng();
    auto v2 = rng();
    auto v3 = rng();
    auto v4 = rng();
    auto v5 = rng();
    auto v6 = rng();
    auto v7 = rng();
    const auto t1 = rdtsc_end();
    do_not_optimize(v0);
    do_not_optimize(v1);
    do_not_optimize(v2);
    do_not_optimize(v3);
    do_not_optimize(v4);
    do_not_optimize(v5);
    do_not_optimize(v6);
    do_not_optimize(v7);
    g_latencies[i] = t1 - t0;
  }
  print_stats("8x operator() batch", compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== Xorshift64 Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n\n", kN);

  print_header();
  bench_xorshift64(cal);
  bench_xorshift64_batch8(cal);

  std::printf("\nTip: taskset -c N ./xorshift64_bench  for stable p99.\n");

  return 0;
}
