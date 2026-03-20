/**
 * @file matching_engine_gbench.cpp
 * @brief Google Benchmark version of MatchingEngine crossing benchmarks.
 *
 * Companion to matching_engine_bench.cpp (custom rdtsc). This version uses
 * Google Benchmark for CI-friendly output, regression tracking, and JSON
 * export.
 *
 * See order_book_gbench.cpp for OrderBook-only benchmarks.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/algo/matching_engine_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/algo/matching_engine_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/algo/matching_engine_gbench
 */

#include "algo/matching_engine.hpp"

#include <benchmark/benchmark.h>

#include <memory>

using namespace mk::algo;

namespace {

// ============================================================================
// Benchmark configuration
// ============================================================================

// Same capacity as custom rdtsc benchmark for comparable results.
using BenchEngine = MatchingEngine<64, 16384, 1024, 16384, 2048>;

// ============================================================================
// Benchmarks
// ============================================================================

// --- submit_order (matching, 5 fills) ---
//
// Full matching loop: opposite ladder scan → 5 FIFO matches →
// remove_filled_order × 5 → level destruction.
//
// Batched: each batch creates a fresh engine to avoid HashMap tombstone
// accumulation. Open-addressing hash maps track tombstones (erased slots)
// in the load factor. After ~capacity×0.7 insert/erase cycles with
// unique keys, tombstones fill the max_load threshold and new inserts
// are rejected. Batching with fresh engines resets the tombstone count.
//
// PauseTiming/ResumeTiming has ~500ns overhead per call, so we amortize
// it across kBatch iterations: pre-place all resting asks for the batch
// upfront, then cross them one-by-one in the timed loop.
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_SubmitOrderMatch(benchmark::State &state) {
  constexpr std::size_t kFillsPerMatch = 5;
  constexpr std::size_t kBatch = 500;

  std::size_t batch_idx = 0;
  OrderId next_id = 1;
  // MatchingEngine is non-movable (owns MmapRegion-backed pools).
  // Use a unique_ptr so we can reconstruct on batch boundaries.
  auto engine = std::make_unique<BenchEngine>();

  // Pre-place first batch of resting asks (before timing starts).
  for (std::size_t i = 0; i < kBatch; ++i) {
    const auto ask_price = static_cast<Price>(200'000 + i);
    for (std::size_t j = 0; j < kFillsPerMatch; ++j) {
      (void)engine->submit_order(next_id++, Side::kAsk, ask_price, 10);
    }
  }

  for (auto _ : state) {
    // Crossing buy — the expensive operation we measure.
    const auto ask_price = static_cast<Price>(200'000 + batch_idx);
    auto result = engine->submit_order(next_id++, Side::kBid, ask_price,
                                       static_cast<Qty>(kFillsPerMatch * 10));
    benchmark::DoNotOptimize(result);

    ++batch_idx;
    if (batch_idx % kBatch == 0) {
      state.PauseTiming();
      engine = std::make_unique<BenchEngine>();
      next_id = 1;
      batch_idx = 0;
      for (std::size_t i = 0; i < kBatch; ++i) {
        const auto ask_price2 = static_cast<Price>(200'000 + i);
        for (std::size_t j = 0; j < kFillsPerMatch; ++j) {
          (void)engine->submit_order(next_id++, Side::kAsk, ask_price2, 10);
        }
      }
      state.ResumeTiming();
    }
  }
}
BENCHMARK(BM_SubmitOrderMatch);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
