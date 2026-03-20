/**
 * @file nano_clock_bench.cpp
 * @brief Microbenchmark for clock function overhead.
 *
 * Measures the cost of each timing source:
 *   - monotonic_nanos()           — CLOCK_MONOTONIC via vDSO (~20ns)
 *   - realtime_nanos()            — CLOCK_REALTIME via vDSO (~20ns)
 *   - rdtsc()                     — raw RDTSC instruction (~1ns)
 *   - rdtsc_start() + rdtsc_end() — serialized TSC pair (~30-40 cycles)
 *
 * Each clock function is called inside a rdtsc_start()/rdtsc_end() bracket
 * to measure its overhead in TSC cycles, then converted to nanoseconds.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/nano_clock_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/nano_clock_bench
 */

#include "bench_utils.hpp"
#include "sys/nano_clock.hpp"

#include <array>
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

void bench_monotonic_nanos(const TscCalibration &cal) {
  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    do_not_optimize(monotonic_nanos());
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    auto val = monotonic_nanos();
    const auto t1 = rdtsc_end();
    do_not_optimize(val);
    g_latencies[i] = t1 - t0;
  }
  print_stats("monotonic_nanos()", compute_stats(cal, g_latencies));
}

void bench_realtime_nanos(const TscCalibration &cal) {
  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    do_not_optimize(realtime_nanos());
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    auto val = realtime_nanos();
    const auto t1 = rdtsc_end();
    do_not_optimize(val);
    g_latencies[i] = t1 - t0;
  }
  print_stats("realtime_nanos()", compute_stats(cal, g_latencies));
}

void bench_rdtsc_raw(const TscCalibration &cal) {
  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    do_not_optimize(rdtsc());
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    auto val = rdtsc();
    const auto t1 = rdtsc_end();
    do_not_optimize(val);
    g_latencies[i] = t1 - t0;
  }
  print_stats("rdtsc() [raw]", compute_stats(cal, g_latencies));
}

void bench_rdtsc_serialized_pair(const TscCalibration &cal) {
  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    auto start = rdtsc_start();
    auto end = rdtsc_end();
    do_not_optimize(start);
    do_not_optimize(end);
  }

  // Measure the cost of a serialized rdtsc_start()+rdtsc_end() pair.
  // We use an outer rdtsc bracket to measure the inner pair.
  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    auto inner_start = rdtsc_start();
    auto inner_end = rdtsc_end();
    const auto t1 = rdtsc_end();
    do_not_optimize(inner_start);
    do_not_optimize(inner_end);
    g_latencies[i] = t1 - t0;
  }
  print_stats("rdtsc_start()+rdtsc_end()",
              compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== NanoClock Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n\n", kN);

  print_header();
  bench_monotonic_nanos(cal);
  bench_realtime_nanos(cal);
  bench_rdtsc_raw(cal);
  bench_rdtsc_serialized_pair(cal);

  std::printf("\nNote: rdtsc() overhead includes rdtsc_start/end bracket cost.\n");
  std::printf("Tip:  taskset -c N ./nano_clock_bench  for stable p99.\n");

  return 0;
}
