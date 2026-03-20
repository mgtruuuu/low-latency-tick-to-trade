/**
 * @file arena_allocator_bench.cpp
 * @brief Microbenchmark for Arena bump-pointer allocation.
 *
 * Measures Arena::alloc() — bump-pointer allocation that should be near-zero
 * cost (pointer add + alignment + boundary check). Arena::reset() is called
 * between batches to reuse the buffer.
 *
 * Uses a stack-allocated buffer (non-owning Arena mode) to avoid mmap overhead
 * in the benchmark setup.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/arena_allocator_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/arena_allocator_bench
 */

#include "bench_utils.hpp"
#include "sys/memory/arena_allocator.hpp"
#include "sys/nano_clock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

using namespace mk::sys;
using namespace mk::sys::memory;
using namespace mk::bench;

namespace {

// ============================================================================
// Configuration
// ============================================================================

constexpr std::size_t kN = 10'000;
constexpr std::size_t kAllocSize = 64; // Cache-line-sized allocations.
constexpr std::size_t kBufSize = 1024UL * 1024; // 1MB arena buffer.

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// Benchmarks
// ============================================================================

void bench_alloc(const TscCalibration &cal) {
  alignas(64) std::byte buf[kBufSize];
  Arena arena(std::span<std::byte>(buf, sizeof(buf)));

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    (void)arena.alloc(kAllocSize, alignof(std::uint64_t));
  }
  arena.reset();

  // Measure in batches, resetting between batches to avoid OOM.
  // Each batch can fit kBufSize / kAllocSize = ~16K allocations.
  constexpr std::size_t kBatch = 1000;
  std::size_t measured = 0;

  while (measured < kN) {
    const std::size_t count = std::min(kBatch, kN - measured);

    for (std::size_t i = 0; i < count; ++i) {
      const auto t0 = rdtsc_start();
      auto *p = arena.alloc(kAllocSize, alignof(std::uint64_t));
      const auto t1 = rdtsc_end();
      do_not_optimize(p);
      g_latencies[measured + i] = t1 - t0;
    }

    arena.reset();
    measured += count;
  }

  print_stats("alloc(64, align=8)", compute_stats(cal, g_latencies));
}

void bench_alloc_cache_aligned(const TscCalibration &cal) {
  alignas(64) std::byte buf[kBufSize];
  Arena arena(std::span<std::byte>(buf, sizeof(buf)));

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    (void)arena.alloc(kAllocSize, 64);
  }
  arena.reset();

  constexpr std::size_t kBatch = 1000;
  std::size_t measured = 0;

  while (measured < kN) {
    const std::size_t count = std::min(kBatch, kN - measured);

    for (std::size_t i = 0; i < count; ++i) {
      const auto t0 = rdtsc_start();
      auto *p = arena.alloc(kAllocSize, 64);
      const auto t1 = rdtsc_end();
      do_not_optimize(p);
      g_latencies[measured + i] = t1 - t0;
    }

    arena.reset();
    measured += count;
  }

  print_stats("alloc(64, align=64)", compute_stats(cal, g_latencies));
}

void bench_reset(const TscCalibration &cal) {
  alignas(64) std::byte buf[kBufSize];
  Arena arena(std::span<std::byte>(buf, sizeof(buf)));

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    (void)arena.alloc(kAllocSize, alignof(std::uint64_t));
    arena.reset();
  }

  for (std::size_t i = 0; i < kN; ++i) {
    // Allocate some data so reset has something to do.
    for (int j = 0; j < 100; ++j) {
      (void)arena.alloc(kAllocSize, alignof(std::uint64_t));
    }

    const auto t0 = rdtsc_start();
    arena.reset();
    const auto t1 = rdtsc_end();
    g_latencies[i] = t1 - t0;
  }

  print_stats("reset()", compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== Arena Allocator Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n\n", kN);

  print_header();
  bench_alloc(cal);
  bench_alloc_cache_aligned(cal);
  bench_reset(cal);

  std::printf("\nNote: Arena alloc is a bump-pointer (near-zero cost).\n");
  std::printf("Tip:  taskset -c N ./arena_allocator_bench  for stable p99.\n");

  return 0;
}
