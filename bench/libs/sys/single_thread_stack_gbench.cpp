/**
 * @file single_thread_stack_gbench.cpp
 * @brief Google Benchmark version of SingleThreadStack benchmarks.
 *
 * Companion to single_thread_stack_bench.cpp (custom rdtsc). This version
 * uses Google Benchmark for CI-friendly output, regression tracking, and
 * JSON export.
 *
 * Zero-synchronization LIFO stack — should be the fastest stack variant.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/single_thread_stack_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/sys/single_thread_stack_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/single_thread_stack_gbench
 */

#include "sys/memory/single_thread_stack.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>

using namespace mk::sys::memory;

namespace {

// ============================================================================
// Configuration
// ============================================================================

constexpr std::size_t kNodeCount = 16384;

// ============================================================================
// Benchmarks
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
