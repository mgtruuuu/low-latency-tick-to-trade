/**
 * @file intrusive_list_gbench.cpp
 * @brief Google Benchmark version of IntrusiveList benchmarks.
 *
 * Companion to intrusive_list_bench.cpp (custom rdtsc). This version uses
 * Google Benchmark for CI-friendly output, regression tracking, and JSON
 * export.
 *
 * Operations benchmarked:
 *   - push_back   — append to tail
 *   - push_front  — prepend to head
 *   - pop_front   — remove from head
 *   - pop_back    — remove from tail
 *   - erase       — remove from middle (O(1) unlink)
 *   - iteration   — walk full list
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/ds/intrusive_list_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/ds/intrusive_list_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/ds/intrusive_list_gbench
 */

#include "ds/intrusive_list.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>

using namespace mk::ds;

namespace {

// ============================================================================
// Configuration
// ============================================================================

constexpr std::size_t kListSize = 1024;

// ============================================================================
// Node type
// ============================================================================

struct Node : IntrusiveListHook {
  std::uint64_t value{0};
};

// ============================================================================
// push_back — append each node, then tear down outside timing.
// Batched: after kBatch push_backs, pause and pop them all to reuse nodes.
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_PushBack(benchmark::State &state) {
  constexpr std::size_t kBatch = 512;
  std::array<Node, kBatch> nodes{};
  IntrusiveList<Node> list;

  std::size_t idx = 0;
  for (auto _ : state) {
    list.push_back(nodes[idx % kBatch]);
    ++idx;
    if (idx % kBatch == 0) {
      state.PauseTiming();
      list.clear();
      state.ResumeTiming();
    }
  }
  // Clean up any remaining nodes.
  list.clear();
}
BENCHMARK(BM_PushBack);

// ============================================================================
// push_front
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_PushFront(benchmark::State &state) {
  constexpr std::size_t kBatch = 512;
  std::array<Node, kBatch> nodes{};
  IntrusiveList<Node> list;

  std::size_t idx = 0;
  for (auto _ : state) {
    list.push_front(nodes[idx % kBatch]);
    ++idx;
    if (idx % kBatch == 0) {
      state.PauseTiming();
      list.clear();
      state.ResumeTiming();
    }
  }
  list.clear();
}
BENCHMARK(BM_PushFront);

// ============================================================================
// pop_front — pre-fill kBatch nodes, pop them all, re-fill.
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_PopFront(benchmark::State &state) {
  constexpr std::size_t kBatch = 512;
  std::array<Node, kBatch> nodes{};
  IntrusiveList<Node> list;

  // Initial fill.
  for (auto &n : nodes) {
    list.push_back(n);
  }

  std::size_t idx = 0;
  for (auto _ : state) {
    auto &node = list.pop_front();
    benchmark::DoNotOptimize(node.value);
    ++idx;
    if (idx % kBatch == 0) {
      state.PauseTiming();
      for (auto &n : nodes) {
        if (!n.is_linked()) {
          list.push_back(n);
        }
      }
      state.ResumeTiming();
    }
  }
  list.clear();
}
BENCHMARK(BM_PopFront);

// ============================================================================
// pop_back
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_PopBack(benchmark::State &state) {
  constexpr std::size_t kBatch = 512;
  std::array<Node, kBatch> nodes{};
  IntrusiveList<Node> list;

  for (auto &n : nodes) {
    list.push_back(n);
  }

  std::size_t idx = 0;
  for (auto _ : state) {
    auto &node = list.pop_back();
    benchmark::DoNotOptimize(node.value);
    ++idx;
    if (idx % kBatch == 0) {
      state.PauseTiming();
      for (auto &n : nodes) {
        if (!n.is_linked()) {
          list.push_back(n);
        }
      }
      state.ResumeTiming();
    }
  }
  list.clear();
}
BENCHMARK(BM_PopBack);

// ============================================================================
// erase — erase from sequential position in list, re-insert outside timing.
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Erase(benchmark::State &state) {
  constexpr std::size_t kBatch = 512;
  std::array<Node, kBatch> nodes{};
  IntrusiveList<Node> list;

  for (auto &n : nodes) {
    list.push_back(n);
  }

  std::size_t idx = 0;
  for (auto _ : state) {
    auto &node = nodes[idx % kBatch];
    list.erase(node);
    ++idx;
    if (idx % kBatch == 0) {
      state.PauseTiming();
      for (auto &n : nodes) {
        if (!n.is_linked()) {
          list.push_back(n);
        }
      }
      state.ResumeTiming();
    }
  }
  list.clear();
}
BENCHMARK(BM_Erase);

// ============================================================================
// iteration — walk a kListSize-element list
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Iterate(benchmark::State &state) {
  std::array<Node, kListSize> nodes{};
  IntrusiveList<Node> list;

  for (std::size_t i = 0; i < kListSize; ++i) {
    nodes[i].value = i;
    list.push_back(nodes[i]);
  }

  for (auto _ : state) {
    std::uint64_t sum = 0;
    for (auto &n : list) {
      sum += n.value;
    }
    benchmark::DoNotOptimize(sum);
  }
  list.clear();
}
BENCHMARK(BM_Iterate);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
