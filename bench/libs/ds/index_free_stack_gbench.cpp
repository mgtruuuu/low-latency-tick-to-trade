/**
 * @file index_free_stack_gbench.cpp
 * @brief Google Benchmark version of IndexFreeStack benchmarks.
 *
 * Companion to index_free_stack_bench.cpp (custom rdtsc). This version uses
 * Google Benchmark for CI-friendly output, regression tracking, and JSON
 * export.
 *
 * Compares two index free stack implementations:
 *   - FixedIndexFreeStack<N> — inline std::array storage
 *   - IndexFreeStack         — caller-managed buffer, runtime capacity
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/ds/index_free_stack_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/ds/index_free_stack_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/ds/index_free_stack_gbench
 */

#include "ds/fixed_index_free_stack.hpp"
#include "ds/index_free_stack.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>

using namespace mk::ds;

namespace {

// ============================================================================
// Configuration
// ============================================================================

constexpr std::size_t kCapacity = 4096;

// ============================================================================
// FixedIndexFreeStack
// ============================================================================

// --- pop ---
// Batched: after kBatch pops, pause and push them all back.
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Fixed_Pop(benchmark::State &state) {
  constexpr std::size_t kBatch = 500;
  FixedIndexFreeStack<kCapacity> stack;
  std::array<std::uint32_t, kBatch> popped{};

  std::size_t count = 0;
  for (auto _ : state) {
    std::uint32_t val = 0;
    (void)stack.pop(val);
    benchmark::DoNotOptimize(val);
    popped[count % kBatch] = val;
    ++count;
    if (count % kBatch == 0) {
      state.PauseTiming();
      for (std::size_t j = 0; j < kBatch; ++j) {
        stack.push(popped[j]);
      }
      state.ResumeTiming();
    }
  }
}
BENCHMARK(BM_Fixed_Pop);

// --- push ---
// Batched: pre-pop kBatch indices, then measure pushing them back.
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Fixed_Push(benchmark::State &state) {
  constexpr std::size_t kBatch = 500;
  FixedIndexFreeStack<kCapacity> stack;
  std::array<std::uint32_t, kBatch> popped{};

  // Pre-pop initial batch.
  for (std::size_t j = 0; j < kBatch; ++j) {
    (void)stack.pop(popped[j]);
  }

  std::size_t count = 0;
  for (auto _ : state) {
    stack.push(popped[count % kBatch]);
    ++count;
    if (count % kBatch == 0) {
      state.PauseTiming();
      for (std::size_t j = 0; j < kBatch; ++j) {
        (void)stack.pop(popped[j]);
      }
      state.ResumeTiming();
    }
  }
}
BENCHMARK(BM_Fixed_Push);

// --- pop+push round-trip ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Fixed_PopPush(benchmark::State &state) {
  FixedIndexFreeStack<kCapacity> stack;

  for (auto _ : state) {
    std::uint32_t idx = 0;
    (void)stack.pop(idx);
    stack.push(idx);
    benchmark::DoNotOptimize(idx);
  }
}
BENCHMARK(BM_Fixed_PopPush);

// ============================================================================
// IndexFreeStack (runtime capacity)
// ============================================================================

constexpr std::size_t kBufSize = IndexFreeStack::required_buffer_size(kCapacity);

// --- pop ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Runtime_Pop(benchmark::State &state) {
  constexpr std::size_t kBatch = 500;
  alignas(64) std::byte buf[kBufSize];
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto stack = IndexFreeStack::create(buf, kBufSize, kCapacity).value();
  std::array<std::uint32_t, kBatch> popped{};

  std::size_t count = 0;
  for (auto _ : state) {
    std::uint32_t val = 0;
    (void)stack.pop(val);
    benchmark::DoNotOptimize(val);
    popped[count % kBatch] = val;
    ++count;
    if (count % kBatch == 0) {
      state.PauseTiming();
      for (std::size_t j = 0; j < kBatch; ++j) {
        stack.push(popped[j]);
      }
      state.ResumeTiming();
    }
  }
}
BENCHMARK(BM_Runtime_Pop);

// --- push ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Runtime_Push(benchmark::State &state) {
  constexpr std::size_t kBatch = 500;
  alignas(64) std::byte buf[kBufSize];
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto stack = IndexFreeStack::create(buf, kBufSize, kCapacity).value();
  std::array<std::uint32_t, kBatch> popped{};

  for (std::size_t j = 0; j < kBatch; ++j) {
    (void)stack.pop(popped[j]);
  }

  std::size_t count = 0;
  for (auto _ : state) {
    stack.push(popped[count % kBatch]);
    ++count;
    if (count % kBatch == 0) {
      state.PauseTiming();
      for (std::size_t j = 0; j < kBatch; ++j) {
        (void)stack.pop(popped[j]);
      }
      state.ResumeTiming();
    }
  }
}
BENCHMARK(BM_Runtime_Push);

// --- pop+push round-trip ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Runtime_PopPush(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize];
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto stack = IndexFreeStack::create(buf, kBufSize, kCapacity).value();

  for (auto _ : state) {
    std::uint32_t idx = 0;
    (void)stack.pop(idx);
    stack.push(idx);
    benchmark::DoNotOptimize(idx);
  }
}
BENCHMARK(BM_Runtime_PopPush);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
