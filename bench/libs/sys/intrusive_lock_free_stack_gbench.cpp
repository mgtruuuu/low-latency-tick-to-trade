/**
 * @file intrusive_lock_free_stack_gbench.cpp
 * @brief Google Benchmark version of IntrusiveLockFreeStack benchmarks.
 *
 * Companion to intrusive_lock_free_stack_bench.cpp (custom rdtsc). This
 * version uses Google Benchmark for CI-friendly output, regression tracking,
 * and JSON export.
 *
 * Intrusive variant: T inherits LockFreeStackHook, same 128-bit CAS.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/intrusive_lock_free_stack_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/sys/intrusive_lock_free_stack_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/intrusive_lock_free_stack_gbench
 */

#include "sys/hardware_constants.hpp"
#include "sys/memory/intrusive_lock_free_stack.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>

using namespace mk::sys::memory;

namespace {

// ============================================================================
// Types
// ============================================================================

struct alignas(mk::sys::kCacheLineSize) Node : LockFreeStackHook {
  std::uint64_t value{0};
};

constexpr std::size_t kNodeCount = 16384;

// ============================================================================
// Benchmarks
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_ILFS_Pop(benchmark::State &state) {
  IntrusiveLockFreeStack<Node> stack;
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
BENCHMARK(BM_ILFS_Pop);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_ILFS_Push(benchmark::State &state) {
  IntrusiveLockFreeStack<Node> stack;
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
BENCHMARK(BM_ILFS_Push);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_ILFS_RoundTrip(benchmark::State &state) {
  IntrusiveLockFreeStack<Node> stack;
  std::array<Node, kNodeCount> nodes{};

  for (auto &n : nodes) {
    stack.push(&n);
  }

  for (auto _ : state) {
    auto *n = stack.try_pop();
    stack.push(n);
  }
}
BENCHMARK(BM_ILFS_RoundTrip);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
