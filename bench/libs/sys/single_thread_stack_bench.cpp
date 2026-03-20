/**
 * @file single_thread_stack_bench.cpp
 * @brief Microbenchmark for SingleThreadStack: push/pop latency.
 *
 * Zero-synchronization LIFO stack for per-core hot paths. Uses plain pointer
 * store instead of 128-bit CMPXCHG16B — expected to be ~20x faster than
 * LockFreeStack on single-threaded workloads.
 *
 * Single-threaded: measures raw operation cost.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/single_thread_stack_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/single_thread_stack_bench
 */

#include "bench_utils.hpp"
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
// Configuration
// ============================================================================

constexpr std::size_t kN = 10'000;
constexpr std::size_t kNodeCount = 16384;

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// Benchmarks
// ============================================================================

void bench_pop(const TscCalibration &cal) {
  using Stack = SingleThreadStack<std::uint64_t>;
  using Node = Stack::NodeType;

  Stack stack;
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
  print_stats("STS: try_pop", compute_stats(cal, g_latencies));
}

void bench_push(const TscCalibration &cal) {
  using Stack = SingleThreadStack<std::uint64_t>;
  using Node = Stack::NodeType;

  Stack stack;
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
  print_stats("STS: push", compute_stats(cal, g_latencies));
}

void bench_round_trip(const TscCalibration &cal) {
  using Stack = SingleThreadStack<std::uint64_t>;
  using Node = Stack::NodeType;

  Stack stack;
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
  print_stats("STS: pop + push round-trip", compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== SingleThreadStack Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n\n", kN);

  print_header();
  bench_pop(cal);
  bench_push(cal);
  bench_round_trip(cal);

  std::printf("\nSTS=SingleThreadStack (zero synchronization)\n");
  std::printf("Tip:  taskset -c N ./single_thread_stack_bench  "
              "for stable p99.\n");

  return 0;
}
