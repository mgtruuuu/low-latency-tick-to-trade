/**
 * @file intrusive_lock_free_stack_bench.cpp
 * @brief Microbenchmark for IntrusiveLockFreeStack: push/pop latency.
 *
 * Intrusive variant of LockFreeStack: T inherits LockFreeStackHook,
 * eliminating the wrapper Node. Same 128-bit CAS (CMPXCHG16B) for ABA
 * prevention. Should show similar latency to LockFreeStack but slightly
 * better cache locality (no wrapper indirection).
 *
 * Single-threaded: measures raw operation cost (no contention).
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/intrusive_lock_free_stack_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/intrusive_lock_free_stack_bench
 */

#include "bench_utils.hpp"
#include "sys/hardware_constants.hpp"
#include "sys/memory/intrusive_lock_free_stack.hpp"
#include "sys/nano_clock.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

using namespace mk::sys;
using namespace mk::sys::memory;
using namespace mk::bench;

namespace {

// ============================================================================
// Types
// ============================================================================

// Intrusive payload: must inherit LockFreeStackHook.
// Cache-line aligned to prevent false sharing between nodes.
struct alignas(kCacheLineSize) Node : LockFreeStackHook {
  std::uint64_t value{0};
};

constexpr std::size_t kN = 10'000;
constexpr std::size_t kNodeCount = 16384;

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// Benchmarks
// ============================================================================

void bench_pop(const TscCalibration &cal) {
  IntrusiveLockFreeStack<Node> stack;
  std::array<Node, kNodeCount> nodes{};

  for (std::size_t i = 0; i < kNodeCount; ++i) {
    stack.push(&nodes[i]);
  }

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    auto *n = stack.try_pop();
    stack.push(n);
  }

  constexpr std::size_t kBatch = 1000;
  std::array<Node *, kBatch> popped{};
  std::size_t measured = 0;

  while (measured < kN) {
    const std::size_t count = std::min(kBatch, kN - measured);
    for (std::size_t i = 0; i < count; ++i) {
      const auto t0 = rdtsc_start();
      popped[i] = stack.try_pop();
      const auto t1 = rdtsc_end();
      g_latencies[measured + i] = t1 - t0;
    }
    for (std::size_t i = 0; i < count; ++i) {
      stack.push(popped[i]);
    }
    measured += count;
  }
  print_stats("ILFS: try_pop", compute_stats(cal, g_latencies));
}

void bench_push(const TscCalibration &cal) {
  IntrusiveLockFreeStack<Node> stack;
  std::array<Node, kNodeCount> nodes{};

  for (std::size_t i = 0; i < kNodeCount; ++i) {
    stack.push(&nodes[i]);
  }

  constexpr std::size_t kBatch = 1000;
  std::array<Node *, kBatch> popped{};
  std::size_t measured = 0;

  while (measured < kN) {
    const std::size_t count = std::min(kBatch, kN - measured);
    for (std::size_t i = 0; i < count; ++i) {
      popped[i] = stack.try_pop();
    }
    for (std::size_t i = 0; i < count; ++i) {
      const auto t0 = rdtsc_start();
      stack.push(popped[i]);
      const auto t1 = rdtsc_end();
      g_latencies[measured + i] = t1 - t0;
    }
    measured += count;
  }
  print_stats("ILFS: push", compute_stats(cal, g_latencies));
}

void bench_round_trip(const TscCalibration &cal) {
  IntrusiveLockFreeStack<Node> stack;
  std::array<Node, kNodeCount> nodes{};

  for (std::size_t i = 0; i < kNodeCount; ++i) {
    stack.push(&nodes[i]);
  }

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    auto *n = stack.try_pop();
    stack.push(n);
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    auto *n = stack.try_pop();
    stack.push(n);
    const auto t1 = rdtsc_end();
    g_latencies[i] = t1 - t0;
  }
  print_stats("ILFS: pop + push round-trip", compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== IntrusiveLockFreeStack Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n\n", kN);

  print_header();
  bench_pop(cal);
  bench_push(cal);
  bench_round_trip(cal);

  std::printf("\nILFS=IntrusiveLockFreeStack (128-bit CAS, intrusive)\n");
  std::printf("Tip:  taskset -c N ./intrusive_lock_free_stack_bench  "
              "for stable p99.\n");

  return 0;
}
