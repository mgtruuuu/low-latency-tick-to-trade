/**
 * @file object_pool_bench.cpp
 * @brief Microbenchmark for ObjectPool allocate/deallocate operations.
 *
 * Compares two free-list policies:
 *   - LockFreeStack<T>    (default) — MPMC, 128-bit CAS (~20 cycles per op)
 *   - SingleThreadStack<T>          — zero synchronization (~1 cycle per op)
 *
 * Single-threaded: measures raw operation cost, not concurrent throughput.
 * This is the relevant metric for per-core hot-path pools.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/object_pool_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/object_pool_bench
 */

#include "bench_utils.hpp"
#include "sys/memory/object_pool.hpp"
#include "sys/memory/single_thread_stack.hpp"
#include "sys/nano_clock.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

using namespace mk::sys;
using namespace mk::sys::memory;
using namespace mk::bench;

namespace {

// ============================================================================
// Benchmark configuration
// ============================================================================

// Cache-line-sized payload — realistic for HFT objects (Order, PriceLevel).
struct alignas(64) Payload {
  std::uint64_t data[8]{};
};

constexpr std::size_t kPoolCapacity = 4096;
constexpr std::size_t kN = 10'000;

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// Benchmarks
// ============================================================================

/// Benchmark allocate(): pop from free list.
/// Pre-fills the pool, then measures single allocate() calls.
/// After each measured batch, deallocates to refill the pool.
template <typename Pool>
void bench_allocate(Pool &pool, const TscCalibration &cal,
                    const char *label) {
  // Warm-up: cycle allocate/deallocate to prime caches.
  std::array<Payload *, 500> warmup_ptrs{};
  for (auto &ptr : warmup_ptrs) {
    ptr = pool.allocate();
  }
  for (auto *ptr : warmup_ptrs) {
    pool.deallocate(ptr);
  }

  // Measure in batches to avoid exhausting the pool.
  constexpr std::size_t kBatch = 1000;
  std::array<Payload *, kBatch> ptrs{};
  std::size_t measured = 0;

  while (measured < kN) {
    const std::size_t count = std::min(kBatch, kN - measured);

    for (std::size_t i = 0; i < count; ++i) {
      const auto t0 = rdtsc_start();
      ptrs[i] = pool.allocate();
      const auto t1 = rdtsc_end();
      g_latencies[measured + i] = t1 - t0;
    }

    // Return all allocated objects.
    for (std::size_t i = 0; i < count; ++i) {
      pool.deallocate(ptrs[i]);
    }
    measured += count;
  }

  print_stats(label, compute_stats(cal, g_latencies));
}

/// Benchmark deallocate(): push to free list.
/// Allocates a batch, then measures single deallocate() calls.
template <typename Pool>
void bench_deallocate(Pool &pool, const TscCalibration &cal,
                      const char *label) {
  constexpr std::size_t kBatch = 1000;
  std::array<Payload *, kBatch> ptrs{};
  std::size_t measured = 0;

  while (measured < kN) {
    const std::size_t count = std::min(kBatch, kN - measured);

    // Setup: allocate objects to deallocate.
    for (std::size_t i = 0; i < count; ++i) {
      ptrs[i] = pool.allocate();
    }

    // Measure deallocate.
    for (std::size_t i = 0; i < count; ++i) {
      const auto t0 = rdtsc_start();
      pool.deallocate(ptrs[i]);
      const auto t1 = rdtsc_end();
      g_latencies[measured + i] = t1 - t0;
    }
    measured += count;
  }

  print_stats(label, compute_stats(cal, g_latencies));
}

/// Benchmark allocate + deallocate round-trip.
template <typename Pool>
void bench_round_trip(Pool &pool, const TscCalibration &cal,
                      const char *label) {
  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    auto *p = pool.allocate();
    pool.deallocate(p);
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    auto *p = pool.allocate();
    pool.deallocate(p);
    const auto t1 = rdtsc_end();
    g_latencies[i] = t1 - t0;
  }

  print_stats(label, compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== ObjectPool Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n\n", kN);

  // --- LockFreeStack policy (default MPMC) ---
  std::printf("Policy: LockFreeStack (128-bit CAS)\n");
  print_header();
  {
    ObjectPool<Payload> pool(kPoolCapacity);
    bench_allocate(pool, cal, "allocate");
    bench_deallocate(pool, cal, "deallocate");
    bench_round_trip(pool, cal, "alloc + dealloc round-trip");
  }

  std::printf("\n");

  // --- SingleThreadStack policy (zero sync) ---
  std::printf("Policy: SingleThreadStack (zero sync)\n");
  print_header();
  {
    ObjectPool<Payload, SingleThreadStack<Payload>> pool(kPoolCapacity);
    bench_allocate(pool, cal, "allocate");
    bench_deallocate(pool, cal, "deallocate");
    bench_round_trip(pool, cal, "alloc + dealloc round-trip");
  }

  std::printf("\nTip: taskset -c N ./object_pool_bench  for stable p99.\n");

  return 0;
}
