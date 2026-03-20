/**
 * @file intrusive_list_bench.cpp
 * @brief Microbenchmark for IntrusiveList operations.
 *
 * Operations benchmarked:
 *   - push_back   — append to tail
 *   - push_front  — prepend to head
 *   - pop_front   — remove from head
 *   - pop_back    — remove from tail
 *   - erase       — remove from middle (O(1) unlink)
 *   - iteration   — walk full list (cache-line traversal)
 *
 * All nodes are pre-allocated in a std::array — zero allocation during
 * measurement, matching the HFT hot-path pattern (ObjectPool + IntrusiveList).
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/ds/intrusive_list_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/ds/intrusive_list_bench
 */

#include "bench_utils.hpp"
#include "ds/intrusive_list.hpp"
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

constexpr std::size_t kN = 10'000;
constexpr std::size_t kListSize = 1024;

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// Node type — inherits IntrusiveListHook for zero-allocation linkage
// ============================================================================

struct Node : IntrusiveListHook {
  std::uint64_t value{0};
};

// Pre-allocated node pools. Each benchmark gets its own pool to avoid
// interference from prior link state (IntrusiveListHook is non-copyable,
// so we need fresh unlinked nodes for each benchmark).
std::array<Node, kN> g_nodes_push_back{};
std::array<Node, kN> g_nodes_push_front{};
std::array<Node, kN> g_nodes_pop_front{};
std::array<Node, kN> g_nodes_pop_back{};
std::array<Node, kN> g_nodes_erase{};
std::array<Node, kListSize> g_nodes_iter{};

// ============================================================================
// Benchmarks
// ============================================================================

void bench_intrusive_list(const TscCalibration &cal) {
  // --- push_back ---
  {
    IntrusiveList<Node> list;

    for (std::size_t i = 0; i < kN; ++i) {
      g_nodes_push_back[i].value = i;
      auto &node = g_nodes_push_back[i];
      const auto t0 = rdtsc_start();
      list.push_back(node);
      const auto t1 = rdtsc_end();
      g_latencies[i] = t1 - t0;
    }
    print_stats("push_back", compute_stats(cal, g_latencies));
  }

  // --- push_front ---
  {
    IntrusiveList<Node> list;

    for (std::size_t i = 0; i < kN; ++i) {
      g_nodes_push_front[i].value = i;
      auto &node = g_nodes_push_front[i];
      const auto t0 = rdtsc_start();
      list.push_front(node);
      const auto t1 = rdtsc_end();
      g_latencies[i] = t1 - t0;
    }
    print_stats("push_front", compute_stats(cal, g_latencies));
  }

  // --- pop_front ---
  {
    IntrusiveList<Node> list;
    for (std::size_t i = 0; i < kN; ++i) {
      g_nodes_pop_front[i].value = i;
      list.push_back(g_nodes_pop_front[i]);
    }

    for (std::size_t i = 0; i < kN; ++i) {
      const auto t0 = rdtsc_start();
      auto &node = list.pop_front();
      const auto t1 = rdtsc_end();
      do_not_optimize(node.value);
      g_latencies[i] = t1 - t0;
    }
    print_stats("pop_front", compute_stats(cal, g_latencies));
  }

  // --- pop_back ---
  {
    IntrusiveList<Node> list;
    for (std::size_t i = 0; i < kN; ++i) {
      g_nodes_pop_back[i].value = i;
      list.push_back(g_nodes_pop_back[i]);
    }

    for (std::size_t i = 0; i < kN; ++i) {
      const auto t0 = rdtsc_start();
      auto &node = list.pop_back();
      const auto t1 = rdtsc_end();
      do_not_optimize(node.value);
      g_latencies[i] = t1 - t0;
    }
    print_stats("pop_back", compute_stats(cal, g_latencies));
  }

  // --- erase (from middle) ---
  // Push all nodes, then measure erasing each one. After each erase,
  // the node is unlinked and we move to the next. The list shrinks
  // but erase is always O(1) regardless of position.
  {
    IntrusiveList<Node> list;
    for (std::size_t i = 0; i < kN; ++i) {
      g_nodes_erase[i].value = i;
      list.push_back(g_nodes_erase[i]);
    }

    for (std::size_t i = 0; i < kN; ++i) {
      auto &node = g_nodes_erase[i];
      const auto t0 = rdtsc_start();
      list.erase(node);
      const auto t1 = rdtsc_end();
      g_latencies[i] = t1 - t0;
    }
    print_stats("erase", compute_stats(cal, g_latencies));
  }

  // --- iteration ---
  // Push kListSize nodes, then measure full-list iteration kN times.
  // This benchmarks cache-line traversal of the linked list.
  {
    IntrusiveList<Node> list;
    for (std::size_t i = 0; i < kListSize; ++i) {
      g_nodes_iter[i].value = i;
      list.push_back(g_nodes_iter[i]);
    }

    for (std::size_t i = 0; i < kN; ++i) {
      std::uint64_t sum = 0;
      const auto t0 = rdtsc_start();
      for (auto &n : list) {
        sum += n.value;
      }
      const auto t1 = rdtsc_end();
      do_not_optimize(sum);
      g_latencies[i] = t1 - t0;
    }
    print_stats("iterate (1024)", compute_stats(cal, g_latencies));
  }
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== IntrusiveList Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n", kN);
  std::printf("List size for iteration: %zu\n\n", kListSize);

  std::printf("IntrusiveList<Node>\n");
  print_header();
  bench_intrusive_list(cal);

  std::printf("\nTip: taskset -c N ./intrusive_list_bench  for stable p99.\n");

  return 0;
}
