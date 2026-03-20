/**
 * @file xorshift64_gbench.cpp
 * @brief Google Benchmark version of Xorshift64 benchmarks.
 *
 * Companion to xorshift64_bench.cpp (custom rdtsc). This version uses
 * Google Benchmark for CI-friendly output, regression tracking, and JSON
 * export.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/xorshift64_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/sys/xorshift64_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/xorshift64_gbench
 */

#include "sys/xorshift64.hpp"

#include <benchmark/benchmark.h>

using namespace mk::sys;

namespace {

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Xorshift64(benchmark::State &state) {
  Xorshift64 rng(0xDEAD'BEEF'CAFE'1234ULL);
  for (auto _ : state) {
    auto val = rng();
    benchmark::DoNotOptimize(val);
  }
}
BENCHMARK(BM_Xorshift64);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Xorshift64_Batch8(benchmark::State &state) {
  Xorshift64 rng(0xDEAD'BEEF'CAFE'5678ULL);
  for (auto _ : state) {
    auto v0 = rng();
    auto v1 = rng();
    auto v2 = rng();
    auto v3 = rng();
    auto v4 = rng();
    auto v5 = rng();
    auto v6 = rng();
    auto v7 = rng();
    benchmark::DoNotOptimize(v0);
    benchmark::DoNotOptimize(v1);
    benchmark::DoNotOptimize(v2);
    benchmark::DoNotOptimize(v3);
    benchmark::DoNotOptimize(v4);
    benchmark::DoNotOptimize(v5);
    benchmark::DoNotOptimize(v6);
    benchmark::DoNotOptimize(v7);
  }
}
BENCHMARK(BM_Xorshift64_Batch8);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
