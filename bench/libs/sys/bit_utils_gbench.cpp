/**
 * @file bit_utils_gbench.cpp
 * @brief Google Benchmark version of bitwise utility benchmarks.
 *
 * Companion to bit_utils_bench.cpp (custom rdtsc). This version uses Google
 * Benchmark for CI-friendly output, regression tracking, and JSON export.
 *
 * Operations: align_up, is_power_of_two, round_up_pow2.
 * All should be single-instruction on x86-64.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/bit_utils_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/sys/bit_utils_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/bit_utils_gbench
 */

#include "sys/bit_utils.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

using namespace mk::sys;

namespace {

// ============================================================================
// Benchmarks
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_AlignUp(benchmark::State &state) {
  std::size_t val = 1;
  for (auto _ : state) {
    auto result = align_up(val, 64);
    benchmark::DoNotOptimize(result);
    ++val;
  }
}
BENCHMARK(BM_AlignUp);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_IsPowerOfTwo(benchmark::State &state) {
  std::size_t val = 1;
  for (auto _ : state) {
    auto result = is_power_of_two(val);
    benchmark::DoNotOptimize(result);
    ++val;
  }
}
BENCHMARK(BM_IsPowerOfTwo);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_RoundUpPow2(benchmark::State &state) {
  std::uint32_t val = 1;
  for (auto _ : state) {
    auto result = round_up_pow2(val);
    benchmark::DoNotOptimize(result);
    ++val;
  }
}
BENCHMARK(BM_RoundUpPow2);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
