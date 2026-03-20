/**
 * @file nano_clock_gbench.cpp
 * @brief Google Benchmark version of NanoClock benchmarks.
 *
 * Companion to nano_clock_bench.cpp (custom rdtsc). This version uses Google
 * Benchmark for CI-friendly output, regression tracking, and JSON export.
 *
 * Measures the overhead of each timing source:
 *   - monotonic_nanos()  — CLOCK_MONOTONIC via vDSO
 *   - realtime_nanos()   — CLOCK_REALTIME via vDSO
 *   - rdtsc()            — raw RDTSC instruction
 *   - rdtsc_start()/rdtsc_end() — serialized TSC pair
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/nano_clock_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/sys/nano_clock_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/nano_clock_gbench
 */

#include "sys/nano_clock.hpp"

#include <benchmark/benchmark.h>

using namespace mk::sys;

namespace {

// ============================================================================
// Benchmarks
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_MonotonicNanos(benchmark::State &state) {
  for (auto _ : state) {
    auto val = monotonic_nanos();
    benchmark::DoNotOptimize(val);
  }
}
BENCHMARK(BM_MonotonicNanos);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_RealtimeNanos(benchmark::State &state) {
  for (auto _ : state) {
    auto val = realtime_nanos();
    benchmark::DoNotOptimize(val);
  }
}
BENCHMARK(BM_RealtimeNanos);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Rdtsc(benchmark::State &state) {
  for (auto _ : state) {
    auto val = rdtsc();
    benchmark::DoNotOptimize(val);
  }
}
BENCHMARK(BM_Rdtsc);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_RdtscSerializedPair(benchmark::State &state) {
  for (auto _ : state) {
    auto start = rdtsc_start();
    auto end = rdtsc_end();
    benchmark::DoNotOptimize(start);
    benchmark::DoNotOptimize(end);
  }
}
BENCHMARK(BM_RdtscSerializedPair);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
