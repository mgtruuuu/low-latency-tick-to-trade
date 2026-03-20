/**
 * @file endian_gbench.cpp
 * @brief Google Benchmark version of endian conversion benchmarks.
 *
 * Companion to endian_bench.cpp (custom rdtsc). This version uses Google
 * Benchmark for CI-friendly output, regression tracking, and JSON export.
 *
 * Operations: host_to_be64, be64_to_host, load_be64, store_be64.
 * On x86-64 (LE host), BE conversion is a single BSWAP instruction.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/endian_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/sys/endian_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/endian_gbench
 */

#include "sys/endian.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

using namespace mk::sys;

namespace {

// ============================================================================
// Benchmarks
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_HostToBe64(benchmark::State &state) {
  std::uint64_t val = 1;
  for (auto _ : state) {
    auto result = host_to_be64(val);
    benchmark::DoNotOptimize(result);
    ++val;
  }
}
BENCHMARK(BM_HostToBe64);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Be64ToHost(benchmark::State &state) {
  std::uint64_t val = 1;
  for (auto _ : state) {
    auto result = be64_to_host(val);
    benchmark::DoNotOptimize(result);
    ++val;
  }
}
BENCHMARK(BM_Be64ToHost);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_LoadBe64(benchmark::State &state) {
  alignas(64) std::byte buf[8]{};
  store_be64(buf, 0xDEADBEEFCAFEBABEULL);

  for (auto _ : state) {
    auto result = load_be64(buf);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_LoadBe64);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_StoreBe64(benchmark::State &state) {
  alignas(64) std::byte buf[8]{};
  std::uint64_t val = 1;

  for (auto _ : state) {
    store_be64(buf, val);
    benchmark::DoNotOptimize(buf);
    ++val;
  }
}
BENCHMARK(BM_StoreBe64);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
