/**
 * @file lock_free_stack_gbench.cpp
 * @brief Google Benchmark version of stack benchmarks.
 *
 * Companion to lock_free_stack_bench.cpp (custom rdtsc). This version uses
 * Google Benchmark for CI-friendly output, regression tracking, and JSON
 * export.
 *
 * Compares three stack implementations:
 *   - LockFreeStack<T>           — MPMC, 128-bit CAS, non-intrusive
 *   - IntrusiveLockFreeStack<T>  — MPMC, 128-bit CAS, intrusive
 *   - SingleThreadStack<T>       — zero synchronization
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/lock_free_stack_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/sys/lock_free_stack_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/lock_free_stack_gbench
 */

#include "sys/memory/intrusive_lock_free_stack.hpp"
#include "sys/memory/lock_free_stack.hpp"
#include "sys/memory/single_thread_stack.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>

using namespace mk::sys::memory;

namespace {

// ============================================================================
// Types
// ============================================================================

struct IntrPayload : LockFreeStackHook {
  std::uint64_t data{0};
};

constexpr std::size_t kNodeCount = 16384;

// ============================================================================
// LockFreeStack
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_LFS_Pop(benchmark::State &state) {
  using Node = LockFreeStack<std::uint64_t>::NodeType;
  LockFreeStack<std::uint64_t> stack;
  std::array<Node, kNodeCount> nodes{};

  for (auto &n : nodes) {
    stack.push(&n);
  }

  for (auto _ : state) {
    auto *n = stack.try_pop();
    benchmark::DoNotOptimize(n);
    stack.push(n);
  }
}
BENCHMARK(BM_LFS_Pop);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_LFS_Push(benchmark::State &state) {
  using Node = LockFreeStack<std::uint64_t>::NodeType;
  LockFreeStack<std::uint64_t> stack;
  std::array<Node, kNodeCount> nodes{};

  for (auto &n : nodes) {
    stack.push(&n);
  }

  for (auto _ : state) {
    auto *n = stack.try_pop();
    stack.push(n);
    benchmark::DoNotOptimize(n);
  }
}
BENCHMARK(BM_LFS_Push);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_LFS_RoundTrip(benchmark::State &state) {
  using Node = LockFreeStack<std::uint64_t>::NodeType;
  LockFreeStack<std::uint64_t> stack;
  std::array<Node, kNodeCount> nodes{};

  for (auto &n : nodes) {
    stack.push(&n);
  }

  for (auto _ : state) {
    auto *n = stack.try_pop();
    stack.push(n);
  }
}
BENCHMARK(BM_LFS_RoundTrip);

// ============================================================================
// IntrusiveLockFreeStack
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_ILFS_Pop(benchmark::State &state) {
  IntrusiveLockFreeStack<IntrPayload> stack;
  std::array<IntrPayload, kNodeCount> nodes{};

  for (auto &n : nodes) {
    stack.push(&n);
  }

  for (auto _ : state) {
    auto *n = stack.try_pop();
    benchmark::DoNotOptimize(n);
    stack.push(n);
  }
}
BENCHMARK(BM_ILFS_Pop);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_ILFS_Push(benchmark::State &state) {
  IntrusiveLockFreeStack<IntrPayload> stack;
  std::array<IntrPayload, kNodeCount> nodes{};

  for (auto &n : nodes) {
    stack.push(&n);
  }

  for (auto _ : state) {
    auto *n = stack.try_pop();
    stack.push(n);
    benchmark::DoNotOptimize(n);
  }
}
BENCHMARK(BM_ILFS_Push);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_ILFS_RoundTrip(benchmark::State &state) {
  IntrusiveLockFreeStack<IntrPayload> stack;
  std::array<IntrPayload, kNodeCount> nodes{};

  for (auto &n : nodes) {
    stack.push(&n);
  }

  for (auto _ : state) {
    auto *n = stack.try_pop();
    stack.push(n);
  }
}
BENCHMARK(BM_ILFS_RoundTrip);

// ============================================================================
// SingleThreadStack
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_STS_Pop(benchmark::State &state) {
  using Node = SingleThreadStack<std::uint64_t>::NodeType;
  SingleThreadStack<std::uint64_t> stack;
  std::array<Node, kNodeCount> nodes{};

  for (auto &n : nodes) {
    stack.push(&n);
  }

  for (auto _ : state) {
    auto *n = stack.try_pop();
    benchmark::DoNotOptimize(n);
    stack.push(n);
  }
}
BENCHMARK(BM_STS_Pop);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_STS_Push(benchmark::State &state) {
  using Node = SingleThreadStack<std::uint64_t>::NodeType;
  SingleThreadStack<std::uint64_t> stack;
  std::array<Node, kNodeCount> nodes{};

  for (auto &n : nodes) {
    stack.push(&n);
  }

  for (auto _ : state) {
    auto *n = stack.try_pop();
    stack.push(n);
    benchmark::DoNotOptimize(n);
  }
}
BENCHMARK(BM_STS_Push);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_STS_RoundTrip(benchmark::State &state) {
  using Node = SingleThreadStack<std::uint64_t>::NodeType;
  SingleThreadStack<std::uint64_t> stack;
  std::array<Node, kNodeCount> nodes{};

  for (auto &n : nodes) {
    stack.push(&n);
  }

  for (auto _ : state) {
    auto *n = stack.try_pop();
    stack.push(n);
  }
}
BENCHMARK(BM_STS_RoundTrip);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
