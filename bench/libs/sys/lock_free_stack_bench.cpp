/**
 * @file lock_free_stack_bench.cpp
 * @brief Microbenchmark for stack implementations: push/pop latency.
 *
 * Compares three stack implementations:
 *   - LockFreeStack<T>           — MPMC, 128-bit CAS, non-intrusive wrapper
 * Node
 *   - IntrusiveLockFreeStack<T>  — MPMC, 128-bit CAS, intrusive (T inherits
 * Hook)
 *   - SingleThreadStack<T>       — zero synchronization, plain pointer store
 *
 * Single-threaded: measures raw operation cost (no contention).
 * LockFreeStack/IntrusiveLockFreeStack should show similar CAS overhead;
 * SingleThreadStack should be ~20x faster (pointer store vs CMPXCHG16B).
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/lock_free_stack_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/lock_free_stack_bench
 */

#include "bench_utils.hpp"
#include "sys/memory/intrusive_lock_free_stack.hpp"
#include "sys/memory/lock_free_stack.hpp"
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
// Types
// ============================================================================

// Intrusive payload: must inherit LockFreeStackHook.
struct IntrPayload : LockFreeStackHook {
  std::uint64_t data{0};
};

constexpr std::size_t kN = 10'000;
constexpr std::size_t kNodeCount = 16384;

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// LockFreeStack benchmarks
// ============================================================================

void bench_lock_free_stack(const TscCalibration &cal) {
  using Stack = LockFreeStack<std::uint64_t>;
  using Node = Stack::NodeType;

  Stack stack;
  // Pre-allocate nodes on the heap (not timed).
  std::array<Node, kNodeCount> nodes{};

  // Push all nodes to set up initial state.
  for (std::size_t i = 0; i < kNodeCount; ++i) {
    stack.push(&nodes[i]);
  }

  // --- pop ---
  // Pop all, then push them back in batches.
  {
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
    print_stats("LFS: try_pop", compute_stats(cal, g_latencies));
  }

  // --- push ---
  {
    constexpr std::size_t kBatch = 1000;
    std::array<Node *, kBatch> popped{};
    std::size_t measured = 0;

    while (measured < kN) {
      const std::size_t count = std::min(kBatch, kN - measured);
      // Setup: pop nodes to push back.
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
    print_stats("LFS: push", compute_stats(cal, g_latencies));
  }

  // --- round-trip ---
  {
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
    print_stats("LFS: pop + push round-trip", compute_stats(cal, g_latencies));
  }
}

// ============================================================================
// IntrusiveLockFreeStack benchmarks
// ============================================================================

void bench_intrusive_lock_free_stack(const TscCalibration &cal) {
  using Stack = IntrusiveLockFreeStack<IntrPayload>;

  Stack stack;
  std::array<IntrPayload, kNodeCount> nodes{};

  for (std::size_t i = 0; i < kNodeCount; ++i) {
    stack.push(&nodes[i]);
  }

  // --- pop ---
  {
    for (int i = 0; i < 500; ++i) {
      auto *n = stack.try_pop();
      stack.push(n);
    }

    constexpr std::size_t kBatch = 1000;
    std::array<IntrPayload *, kBatch> popped{};
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

  // --- push ---
  {
    constexpr std::size_t kBatch = 1000;
    std::array<IntrPayload *, kBatch> popped{};
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

  // --- round-trip ---
  {
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
}

// ============================================================================
// SingleThreadStack benchmarks
// ============================================================================

void bench_single_thread_stack(const TscCalibration &cal) {
  using Stack = SingleThreadStack<std::uint64_t>;
  using Node = Stack::NodeType;

  Stack stack;
  std::array<Node, kNodeCount> nodes{};

  for (std::size_t i = 0; i < kNodeCount; ++i) {
    stack.push(&nodes[i]);
  }

  // --- pop ---
  {
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
    print_stats("STS: try_pop", compute_stats(cal, g_latencies));
  }

  // --- push ---
  {
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
    print_stats("STS: push", compute_stats(cal, g_latencies));
  }

  // --- round-trip ---
  {
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
    print_stats("STS: pop + push round-trip", compute_stats(cal, g_latencies));
  }
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== Stack Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n\n", kN);

  print_header();
  bench_lock_free_stack(cal);
  std::printf("\n");
  bench_intrusive_lock_free_stack(cal);
  std::printf("\n");
  bench_single_thread_stack(cal);

  std::printf("\nLFS=LockFreeStack  ILFS=IntrusiveLockFreeStack  "
              "STS=SingleThreadStack\n");
  std::printf("Tip:  taskset -c N ./lock_free_stack_bench  for stable p99.\n");

  return 0;
}
