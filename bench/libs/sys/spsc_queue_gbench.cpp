/**
 * @file spsc_queue_gbench.cpp
 * @brief Google Benchmark version of SPSC queue benchmarks.
 *
 * Companion to spsc_queue_bench.cpp (custom rdtsc). This version uses Google
 * Benchmark for CI-friendly output, regression tracking, and JSON export.
 *
 * Compares two SPSC queue implementations:
 *   - FixedSPSCQueue<T, N> — compile-time capacity, inline std::array
 *   - SPSCQueue<T>         — runtime capacity, MmapRegion-backed
 *
 * Single-threaded: measures raw try_push/try_pop latency.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/spsc_queue_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/sys/spsc_queue_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/spsc_queue_gbench
 */

#include "sys/memory/fixed_spsc_queue.hpp"
#include "sys/memory/mmap_utils.hpp"
#include "sys/memory/spsc_queue.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>

using namespace mk::sys::memory;

namespace {

// ============================================================================
// Configuration
// ============================================================================

constexpr std::uint32_t kQueueCapacity = 4096;

// ============================================================================
// FixedSPSCQueue
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Fixed_Push(benchmark::State &state) {
  FixedSPSCQueue<std::uint64_t, kQueueCapacity> q;

  // Pre-fill to ~50%.
  for (std::uint32_t i = 0; i < kQueueCapacity / 2; ++i) {
    (void)q.try_push(static_cast<std::uint64_t>(i));
  }

  std::uint64_t val = 0;
  for (auto _ : state) {
    (void)q.try_push(val++);
    std::uint64_t tmp = 0;
    (void)q.try_pop(tmp);
  }
}
BENCHMARK(BM_Fixed_Push);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Fixed_Pop(benchmark::State &state) {
  FixedSPSCQueue<std::uint64_t, kQueueCapacity> q;

  for (std::uint32_t i = 0; i < kQueueCapacity / 2; ++i) {
    (void)q.try_push(static_cast<std::uint64_t>(i));
  }

  std::uint64_t val = 0;
  for (auto _ : state) {
    std::uint64_t tmp = 0;
    (void)q.try_pop(tmp);
    benchmark::DoNotOptimize(tmp);
    (void)q.try_push(val++);
  }
}
BENCHMARK(BM_Fixed_Pop);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Fixed_RoundTrip(benchmark::State &state) {
  FixedSPSCQueue<std::uint64_t, kQueueCapacity> q;

  std::uint64_t val = 0;
  for (auto _ : state) {
    (void)q.try_push(val++);
    std::uint64_t tmp = 0;
    (void)q.try_pop(tmp);
    benchmark::DoNotOptimize(tmp);
  }
}
BENCHMARK(BM_Fixed_RoundTrip);

// ============================================================================
// SPSCQueue (runtime capacity)
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Runtime_Push(benchmark::State &state) {
  auto region = allocate_hot_rw_region(
      {.size = SPSCQueue<std::uint64_t>::required_buffer_size(kQueueCapacity)});
  SPSCQueue<std::uint64_t> q(region.get(), region.size(), kQueueCapacity);

  for (std::uint32_t i = 0; i < kQueueCapacity / 2; ++i) {
    (void)q.try_push(static_cast<std::uint64_t>(i));
  }

  std::uint64_t val = 0;
  for (auto _ : state) {
    (void)q.try_push(val++);
    std::uint64_t tmp = 0;
    (void)q.try_pop(tmp);
  }
}
BENCHMARK(BM_Runtime_Push);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Runtime_Pop(benchmark::State &state) {
  auto region = allocate_hot_rw_region(
      {.size = SPSCQueue<std::uint64_t>::required_buffer_size(kQueueCapacity)});
  SPSCQueue<std::uint64_t> q(region.get(), region.size(), kQueueCapacity);

  for (std::uint32_t i = 0; i < kQueueCapacity / 2; ++i) {
    (void)q.try_push(static_cast<std::uint64_t>(i));
  }

  std::uint64_t val = 0;
  for (auto _ : state) {
    std::uint64_t tmp = 0;
    (void)q.try_pop(tmp);
    benchmark::DoNotOptimize(tmp);
    (void)q.try_push(val++);
  }
}
BENCHMARK(BM_Runtime_Pop);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Runtime_RoundTrip(benchmark::State &state) {
  auto region = allocate_hot_rw_region(
      {.size = SPSCQueue<std::uint64_t>::required_buffer_size(kQueueCapacity)});
  SPSCQueue<std::uint64_t> q(region.get(), region.size(), kQueueCapacity);

  std::uint64_t val = 0;
  for (auto _ : state) {
    (void)q.try_push(val++);
    std::uint64_t tmp = 0;
    (void)q.try_pop(tmp);
    benchmark::DoNotOptimize(tmp);
  }
}
BENCHMARK(BM_Runtime_RoundTrip);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
