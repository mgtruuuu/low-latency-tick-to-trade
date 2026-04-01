/**
 * @file matching_engine_bench.cpp
 * @brief Microbenchmark for MatchingEngine crossing operations.
 *
 * Measures per-operation latency using serialized rdtsc (CPU cycle counter).
 * Reports min, median, p99, and max in nanoseconds.
 *
 * See order_book_bench.cpp for OrderBook-only benchmarks.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/algo/matching_engine_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/algo/matching_engine_bench
 */

#include "algo/matching_engine.hpp"
#include "bench_utils.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace mk::algo;
using namespace mk::sys;
using namespace mk::bench;

namespace {

// ============================================================================
// Benchmark configuration
// ============================================================================

constexpr OrderBook::Params kBenchParams{
    .max_orders = 16384,
    .max_levels = 1024,
    .order_map_cap = 16384,
    .level_map_cap = 2048,
};
using BenchEngine = MatchingEngine<64>;

constexpr std::size_t kN = 10'000;

// Global latency buffer. 10K × 8 bytes = 80KB.
std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// Benchmarks
// ============================================================================

// --- submit_order (matching, 5 fills) ---
//
// Full matching loop: opposite ladder scan → 5 FIFO matches →
// remove_filled_order × 5 → level destruction.
//
// Each iteration: place 5 asks at a unique price, then cross with a
// buy that fully fills all 5. Matched orders are removed from the book.
//
// Batched: each batch creates a fresh engine to avoid HashMap tombstone
// accumulation. Open-addressing hash maps track tombstones (erased slots)
// in the load factor. After ~capacity×0.7 insert/erase cycles with
// unique keys, tombstones fill the max_load threshold and new inserts
// are rejected. Batching with fresh engines resets the tombstone count.
void bench_match(const TscCalibration &cal) {
  constexpr std::size_t kFillsPerMatch = 5;
  constexpr std::size_t kBatch = 1000;
  std::size_t measured = 0;

  // Buffer allocated once, reused across batches by constructing new engines.
  std::vector<std::byte> buf(BenchEngine::required_buffer_size(kBenchParams));

  while (measured < kN) {
    // Construct a fresh engine on the same buffer.
    // clear() in init() resets all state including tombstones.
    BenchEngine engine(buf.data(), buf.size(), kBenchParams);
    OrderId next_id = 1;
    const std::size_t count = std::min(kBatch, kN - measured);

    // Warm-up: 100 matches to prime caches.
    for (std::size_t i = 0; i < 100; ++i) {
      const auto ask_price = static_cast<Price>(100'000 + i);
      for (std::size_t j = 0; j < kFillsPerMatch; ++j) {
        (void)engine.submit_order(next_id++, Side::kAsk, ask_price, 10);
      }
      (void)engine.submit_order(next_id++, Side::kBid, ask_price,
                                static_cast<Qty>(kFillsPerMatch * 10));
    }

    // Measure: only the crossing buy is timed.
    for (std::size_t i = 0; i < count; ++i) {
      const auto ask_price = static_cast<Price>(200'000 + i);

      // Place resting asks (setup — not timed).
      for (std::size_t j = 0; j < kFillsPerMatch; ++j) {
        (void)engine.submit_order(next_id++, Side::kAsk, ask_price, 10);
      }

      // Crossing buy — this is what we measure.
      const OrderId buy_id = next_id++;
      const auto t0 = rdtsc_start();
      auto result = engine.submit_order(buy_id, Side::kBid, ask_price,
                                        static_cast<Qty>(kFillsPerMatch * 10));
      const auto t1 = rdtsc_end();
      g_latencies[measured + i] = t1 - t0;

      // Sanity: must be fully filled with exactly kFillsPerMatch fills.
      if (result.remaining_qty != 0 || result.fills.size() != kFillsPerMatch) {
        std::fprintf(stderr,
                     "ERROR: match failed at i=%zu (remaining=%u, fills=%zu)\n",
                     measured + i, result.remaining_qty, result.fills.size());
        std::abort();
      }
    }
    measured += count;
  }

  print_stats("submit_order (5 fills)", compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== MatchingEngine Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n\n", kN);

  print_header();
  bench_match(cal);

  std::printf("\nTip: taskset -c N ./matching_engine_bench  for stable p99.\n");

  return 0;
}
