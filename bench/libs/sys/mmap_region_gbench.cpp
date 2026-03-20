/**
 * @file mmap_region_gbench.cpp
 * @brief Google Benchmark version of MmapRegion benchmarks.
 *
 * Companion to mmap_region_bench.cpp (custom rdtsc). This version uses Google
 * Benchmark for CI-friendly output, regression tracking, and JSON export.
 *
 * Cold-path component: measures mmap/munmap syscall overhead for anonymous
 * mappings at 4KB and 2MB sizes.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/mmap_region_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/sys/mmap_region_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/mmap_region_gbench
 */

#include "sys/memory/mmap_region.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>

using namespace mk::sys::memory;

namespace {

// ============================================================================
// Configuration
// ============================================================================

constexpr std::size_t kSize4K = 4096;
constexpr std::size_t kSize2M = 2UL * 1024 * 1024;

// ============================================================================
// Benchmarks
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Anonymous4K(benchmark::State &state) {
  for (auto _ : state) {
    auto r = MmapRegion::allocate_anonymous(kSize4K);
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Anonymous4K);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Anonymous2M(benchmark::State &state) {
  for (auto _ : state) {
    auto r = MmapRegion::allocate_anonymous(kSize2M);
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Anonymous2M);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
