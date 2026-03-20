/**
 * @file arena_allocator_gbench.cpp
 * @brief Google Benchmark version of Arena allocator benchmarks.
 *
 * Companion to arena_allocator_bench.cpp (custom rdtsc). This version uses
 * Google Benchmark for CI-friendly output, regression tracking, and JSON
 * export.
 *
 * Measures Arena::alloc() bump-pointer allocation cost. Uses stack-allocated
 * buffer (non-owning mode) to isolate allocation overhead.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/arena_allocator_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/sys/arena_allocator_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/arena_allocator_gbench
 */

#include "sys/memory/arena_allocator.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <span>

using namespace mk::sys::memory;

namespace {

// ============================================================================
// Configuration
// ============================================================================

constexpr std::size_t kAllocSize = 64;
constexpr std::size_t kBufSize = 1024UL * 1024; // 1MB
constexpr std::size_t kBatch = 1000;

// ============================================================================
// Benchmarks
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_ArenaAlloc(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize];
  Arena arena(std::span<std::byte>(buf, sizeof(buf)));
  std::size_t idx = 0;

  for (auto _ : state) {
    auto *p = arena.alloc(kAllocSize, alignof(std::uint64_t));
    benchmark::DoNotOptimize(p);
    ++idx;
    if (idx == kBatch) {
      state.PauseTiming();
      arena.reset();
      idx = 0;
      state.ResumeTiming();
    }
  }
}
BENCHMARK(BM_ArenaAlloc);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_ArenaAllocCacheAligned(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize];
  Arena arena(std::span<std::byte>(buf, sizeof(buf)));
  std::size_t idx = 0;

  for (auto _ : state) {
    auto *p = arena.alloc(kAllocSize, 64);
    benchmark::DoNotOptimize(p);
    ++idx;
    if (idx == kBatch) {
      state.PauseTiming();
      arena.reset();
      idx = 0;
      state.ResumeTiming();
    }
  }
}
BENCHMARK(BM_ArenaAllocCacheAligned);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_ArenaReset(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize];
  Arena arena(std::span<std::byte>(buf, sizeof(buf)));

  for (auto _ : state) {
    state.PauseTiming();
    for (std::size_t j = 0; j < 100; ++j) {
      (void)arena.alloc(kAllocSize, alignof(std::uint64_t));
    }
    state.ResumeTiming();

    arena.reset();
  }
}
BENCHMARK(BM_ArenaReset);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
