/**
 * @file object_pool_gbench.cpp
 * @brief Google Benchmark version of ObjectPool benchmarks.
 *
 * Companion to object_pool_bench.cpp (custom rdtsc). This version uses Google
 * Benchmark for CI-friendly output, regression tracking, and JSON export.
 *
 * Compares two free-list policies:
 *   - LockFreeStack<T>    (default) — MPMC, 128-bit CAS
 *   - SingleThreadStack<T>          — zero synchronization
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/object_pool_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/sys/object_pool_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/object_pool_gbench
 */

#include "sys/memory/object_pool.hpp"
#include "sys/memory/single_thread_stack.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>

using namespace mk::sys::memory;

namespace {

// ============================================================================
// Configuration
// ============================================================================

// Cache-line-sized payload — realistic for HFT objects.
struct alignas(64) Payload {
  std::uint64_t data[8]{};
};

constexpr std::size_t kPoolCapacity = 4096;

// ============================================================================
// LockFreeStack policy (default MPMC)
// ============================================================================

// --- allocate ---
// Batched: deallocate every kBatch iterations to avoid pool exhaustion.
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_LFS_Allocate(benchmark::State &state) {
  ObjectPool<Payload> pool(kPoolCapacity);
  constexpr std::size_t kBatch = 1000;
  Payload *ptrs[kBatch];
  std::size_t idx = 0;

  for (auto _ : state) {
    ptrs[idx] = pool.allocate();
    benchmark::DoNotOptimize(ptrs[idx]);
    ++idx;
    if (idx == kBatch) {
      state.PauseTiming();
      for (auto *ptr : ptrs) {
        pool.deallocate(ptr);
      }
      idx = 0;
      state.ResumeTiming();
    }
  }

  // Clean up remaining.
  for (std::size_t i = 0; i < idx; ++i) {
    pool.deallocate(ptrs[i]);
  }
}
BENCHMARK(BM_LFS_Allocate);

// --- deallocate ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_LFS_Deallocate(benchmark::State &state) {
  ObjectPool<Payload> pool(kPoolCapacity);
  constexpr std::size_t kBatch = 1000;
  Payload *ptrs[kBatch];
  std::size_t idx = 0;

  // Pre-allocate a batch.
  for (auto &ptr : ptrs) {
    ptr = pool.allocate();
  }

  for (auto _ : state) {
    pool.deallocate(ptrs[idx]);
    ++idx;
    if (idx == kBatch) {
      state.PauseTiming();
      for (auto &ptr : ptrs) {
        ptr = pool.allocate();
      }
      idx = 0;
      state.ResumeTiming();
    }
  }
}
BENCHMARK(BM_LFS_Deallocate);

// --- round-trip ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_LFS_RoundTrip(benchmark::State &state) {
  ObjectPool<Payload> pool(kPoolCapacity);

  for (auto _ : state) {
    auto *p = pool.allocate();
    pool.deallocate(p);
  }
}
BENCHMARK(BM_LFS_RoundTrip);

// ============================================================================
// SingleThreadStack policy (zero sync)
// ============================================================================

using STSPool = ObjectPool<Payload, SingleThreadStack<Payload>>;

// --- allocate ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_STS_Allocate(benchmark::State &state) {
  STSPool pool(kPoolCapacity);
  constexpr std::size_t kBatch = 1000;
  Payload *ptrs[kBatch];
  std::size_t idx = 0;

  for (auto _ : state) {
    ptrs[idx] = pool.allocate();
    benchmark::DoNotOptimize(ptrs[idx]);
    ++idx;
    if (idx == kBatch) {
      state.PauseTiming();
      for (auto *ptr : ptrs) {
        pool.deallocate(ptr);
      }
      idx = 0;
      state.ResumeTiming();
    }
  }

  for (std::size_t i = 0; i < idx; ++i) {
    pool.deallocate(ptrs[i]);
  }
}
BENCHMARK(BM_STS_Allocate);

// --- deallocate ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_STS_Deallocate(benchmark::State &state) {
  STSPool pool(kPoolCapacity);
  constexpr std::size_t kBatch = 1000;
  Payload *ptrs[kBatch];
  std::size_t idx = 0;

  for (auto &ptr : ptrs) {
    ptr = pool.allocate();
  }

  for (auto _ : state) {
    pool.deallocate(ptrs[idx]);
    ++idx;
    if (idx == kBatch) {
      state.PauseTiming();
      for (auto &ptr : ptrs) {
        ptr = pool.allocate();
      }
      idx = 0;
      state.ResumeTiming();
    }
  }
}
BENCHMARK(BM_STS_Deallocate);

// --- round-trip ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_STS_RoundTrip(benchmark::State &state) {
  STSPool pool(kPoolCapacity);

  for (auto _ : state) {
    auto *p = pool.allocate();
    pool.deallocate(p);
  }
}
BENCHMARK(BM_STS_RoundTrip);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
