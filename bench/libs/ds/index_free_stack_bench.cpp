/**
 * @file index_free_stack_bench.cpp
 * @brief Microbenchmark for FixedIndexFreeStack and IndexFreeStack operations.
 *
 * Compares two index free stack implementations:
 *   - FixedIndexFreeStack<N> — inline std::array storage, compile-time capacity
 *   - IndexFreeStack         — caller-managed buffer, runtime capacity
 *
 * Operations benchmarked:
 *   - pop          — allocate an index
 *   - push         — return an index
 *   - pop+push     — round-trip (steady-state alloc/free)
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/ds/index_free_stack_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/ds/index_free_stack_bench
 */

#include "bench_utils.hpp"
#include "ds/fixed_index_free_stack.hpp"
#include "ds/index_free_stack.hpp"
#include "sys/nano_clock.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

using namespace mk::sys;
using namespace mk::ds;
using namespace mk::bench;

namespace {

// ============================================================================
// Benchmark configuration
// ============================================================================

constexpr std::size_t kCapacity = 4096;
constexpr std::size_t kN = 10'000;

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// FixedIndexFreeStack benchmarks
// ============================================================================

void bench_fixed_index_free_stack(const TscCalibration &cal) {
  // --- pop ---
  // Batched: after kBatch pops, push them all back to refill.
  {
    FixedIndexFreeStack<kCapacity> stack;
    std::array<std::uint32_t, kCapacity> popped{};

    constexpr std::size_t kBatch = 500;
    std::size_t measured = 0;

    while (measured < kN) {
      const std::size_t count = std::min(kBatch, kN - measured);

      for (std::size_t i = 0; i < count; ++i) {
        const auto t0 = rdtsc_start();
        auto idx = stack.pop();
        const auto t1 = rdtsc_end();
        do_not_optimize(idx);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        popped[i] = *idx;
        g_latencies[measured + i] = t1 - t0;
      }

      // Push back all popped indices.
      for (std::size_t i = 0; i < count; ++i) {
        stack.push(popped[i]);
      }
      measured += count;
    }
    print_stats("Fixed: pop", compute_stats(cal, g_latencies));
  }

  // --- push ---
  // Pop kBatch indices first, then measure pushing them back.
  {
    FixedIndexFreeStack<kCapacity> stack;
    std::array<std::uint32_t, kCapacity> popped{};

    constexpr std::size_t kBatch = 500;
    std::size_t measured = 0;

    while (measured < kN) {
      const std::size_t count = std::min(kBatch, kN - measured);

      // Pre-pop.
      for (std::size_t i = 0; i < count; ++i) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        popped[i] = *stack.pop();
      }

      // Measure push.
      for (std::size_t i = 0; i < count; ++i) {
        const auto t0 = rdtsc_start();
        stack.push(popped[i]);
        const auto t1 = rdtsc_end();
        g_latencies[measured + i] = t1 - t0;
      }
      measured += count;
    }
    print_stats("Fixed: push", compute_stats(cal, g_latencies));
  }

  // --- pop+push round-trip ---
  {
    FixedIndexFreeStack<kCapacity> stack;

    for (std::size_t i = 0; i < kN; ++i) {
      const auto t0 = rdtsc_start();
      auto idx = stack.pop();
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      stack.push(*idx);
      const auto t1 = rdtsc_end();
      do_not_optimize(idx);
      g_latencies[i] = t1 - t0;
    }
    print_stats("Fixed: pop+push", compute_stats(cal, g_latencies));
  }
}

// ============================================================================
// IndexFreeStack benchmarks
// ============================================================================

void bench_index_free_stack(const TscCalibration &cal) {
  constexpr std::size_t kBufSize = IndexFreeStack::required_buffer_size(kCapacity);
  alignas(64) static std::byte buf_pop[kBufSize];
  alignas(64) static std::byte buf_push[kBufSize];
  alignas(64) static std::byte buf_roundtrip[kBufSize];

  // --- pop ---
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto stack = IndexFreeStack::create(buf_pop, kBufSize, kCapacity).value();
    std::array<std::uint32_t, kCapacity> popped{};

    constexpr std::size_t kBatch = 500;
    std::size_t measured = 0;

    while (measured < kN) {
      const std::size_t count = std::min(kBatch, kN - measured);

      for (std::size_t i = 0; i < count; ++i) {
        const auto t0 = rdtsc_start();
        auto idx = stack.pop();
        const auto t1 = rdtsc_end();
        do_not_optimize(idx);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        popped[i] = *idx;
        g_latencies[measured + i] = t1 - t0;
      }

      for (std::size_t i = 0; i < count; ++i) {
        stack.push(popped[i]);
      }
      measured += count;
    }
    print_stats("Runtime: pop", compute_stats(cal, g_latencies));
  }

  // --- push ---
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto stack = IndexFreeStack::create(buf_push, kBufSize, kCapacity).value();
    std::array<std::uint32_t, kCapacity> popped{};

    constexpr std::size_t kBatch = 500;
    std::size_t measured = 0;

    while (measured < kN) {
      const std::size_t count = std::min(kBatch, kN - measured);

      for (std::size_t i = 0; i < count; ++i) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        popped[i] = *stack.pop();
      }

      for (std::size_t i = 0; i < count; ++i) {
        const auto t0 = rdtsc_start();
        stack.push(popped[i]);
        const auto t1 = rdtsc_end();
        g_latencies[measured + i] = t1 - t0;
      }
      measured += count;
    }
    print_stats("Runtime: push", compute_stats(cal, g_latencies));
  }

  // --- pop+push round-trip ---
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto stack = IndexFreeStack::create(buf_roundtrip, kBufSize, kCapacity).value();

    for (std::size_t i = 0; i < kN; ++i) {
      const auto t0 = rdtsc_start();
      auto idx = stack.pop();
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      stack.push(*idx);
      const auto t1 = rdtsc_end();
      do_not_optimize(idx);
      g_latencies[i] = t1 - t0;
    }
    print_stats("Runtime: pop+push", compute_stats(cal, g_latencies));
  }
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== IndexFreeStack Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n", kN);
  std::printf("Capacity: %zu\n\n", kCapacity);

  std::printf("FixedIndexFreeStack<%zu>\n", kCapacity);
  print_header();
  bench_fixed_index_free_stack(cal);

  std::printf("\n");

  std::printf("IndexFreeStack (capacity %zu)\n", kCapacity);
  print_header();
  bench_index_free_stack(cal);

  std::printf("\nTip: taskset -c N ./index_free_stack_bench  for stable p99.\n");

  return 0;
}
