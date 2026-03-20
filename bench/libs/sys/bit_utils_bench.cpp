/**
 * @file bit_utils_bench.cpp
 * @brief Microbenchmark for bitwise utility functions.
 *
 * Operations benchmarked:
 *   - align_up()         — (value + align-1) & ~(align-1)
 *   - is_power_of_two()  — std::has_single_bit / n & (n-1) trick
 *   - round_up_pow2()    — std::bit_ceil / CLZ-based
 *
 * All operations should be single-instruction on x86-64. The benchmark
 * mainly confirms zero overhead and prevents regressions.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/bit_utils_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/bit_utils_bench
 */

#include "bench_utils.hpp"
#include "sys/bit_utils.hpp"
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

void bench_align_up(const TscCalibration &cal) {
  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    do_not_optimize(
        align_up(static_cast<std::size_t>(i | 1), 64));
  }

  for (std::size_t i = 0; i < kN; ++i) {
    // Varying input to prevent constant folding.
    const auto input = static_cast<std::size_t>(i | 1);
    const auto t0 = rdtsc_start();
    auto result = align_up(input, 64);
    const auto t1 = rdtsc_end();
    do_not_optimize(result);
    g_latencies[i] = t1 - t0;
  }
  print_stats("align_up(x, 64)", compute_stats(cal, g_latencies));
}

void bench_is_power_of_two(const TscCalibration &cal) {
  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    do_not_optimize(
        is_power_of_two(static_cast<std::size_t>(i | 1)));
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto input = static_cast<std::size_t>(i | 1);
    const auto t0 = rdtsc_start();
    auto result = is_power_of_two(input);
    const auto t1 = rdtsc_end();
    do_not_optimize(result);
    g_latencies[i] = t1 - t0;
  }
  print_stats("is_power_of_two()", compute_stats(cal, g_latencies));
}

void bench_round_up_pow2(const TscCalibration &cal) {
  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    do_not_optimize(
        round_up_pow2(static_cast<std::uint32_t>(i | 1)));
  }

  for (std::size_t i = 0; i < kN; ++i) {
    // Ensure non-zero input and prevent constant folding.
    const auto input = static_cast<std::uint32_t>(i | 1);
    const auto t0 = rdtsc_start();
    auto result = round_up_pow2(input);
    const auto t1 = rdtsc_end();
    do_not_optimize(result);
    g_latencies[i] = t1 - t0;
  }
  print_stats("round_up_pow2()", compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== BitUtils Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n\n", kN);

  print_header();
  bench_align_up(cal);
  bench_is_power_of_two(cal);
  bench_round_up_pow2(cal);

  std::printf("\nNote: All ops should be single-instruction on x86-64.\n");
  std::printf("Tip:  taskset -c N ./bit_utils_bench  for stable p99.\n");

  return 0;
}
