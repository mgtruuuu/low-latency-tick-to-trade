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

#include <cstddef>
#include <memory>
#include <vector>

#include <benchmark/benchmark.h>

using namespace mk::algo;

namespace {

// ============================================================================
// Benchmark configuration
// ============================================================================

// Same capacity as custom rdtsc benchmark for comparable results.
constexpr OrderBook::Params kBenchParams{
    .max_orders = 16384,
    .max_levels = 1024,
    .order_map_cap = 16384,
    .level_map_cap = 2048,
};
using BenchEngine = MatchingEngine<64>;

// ============================================================================
// Helper — owns a buffer + engine pair for easy reconstruction.
// ============================================================================
//
// MatchingEngine is non-movable (IntrusiveList sentinel has address identity).
// For batch boundaries we need to reconstruct the engine. unique_ptr<Engine>
// allows reset-in-place via destroy + placement-new on the same allocation.

struct OwnedEngine {
  std::vector<std::byte> buf;
  std::unique_ptr<BenchEngine> engine;

  OwnedEngine()
      : buf(BenchEngine::required_buffer_size(kBenchParams)),
        engine(std::make_unique<BenchEngine>(buf.data(), buf.size(),
                                             kBenchParams)) {}

  void reset() {
    engine =
        std::make_unique<BenchEngine>(buf.data(), buf.size(), kBenchParams);
  }
};

// ============================================================================
// Benchmarks
// ============================================================================

// --- submit_order (matching, 5 fills) ---
//
// Full matching loop: opposite ladder scan → 5 FIFO matches →
// remove_filled_order × 5 → level destruction.
//
// Batched: each batch creates a fresh engine to avoid HashMap tombstone
// accumulation. PauseTiming/ResumeTiming has ~500ns overhead per call, so
// we amortize it across kBatch iterations.
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_SubmitOrderMatch(benchmark::State &state) {
  constexpr std::size_t kFillsPerMatch = 5;
  constexpr std::size_t kBatch = 500;

  std::size_t batch_idx = 0;
  OrderId next_id = 1;
  OwnedEngine owned;

  // Pre-place first batch of resting asks (before timing starts).
  for (std::size_t i = 0; i < kBatch; ++i) {
    const auto ask_price = static_cast<Price>(200'000 + i);
    for (std::size_t j = 0; j < kFillsPerMatch; ++j) {
      (void)owned.engine->submit_order(next_id++, Side::kAsk, ask_price, 10);
    }
  }

  for (auto _ : state) {
    // Crossing buy — the expensive operation we measure.
    const auto ask_price = static_cast<Price>(200'000 + batch_idx);
    auto result =
        owned.engine->submit_order(next_id++, Side::kBid, ask_price,
                                   static_cast<Qty>(kFillsPerMatch * 10));
    benchmark::DoNotOptimize(result);

    ++batch_idx;
    if (batch_idx % kBatch == 0) {
      state.PauseTiming();
      owned.reset();
      next_id = 1;
      batch_idx = 0;
      for (std::size_t i = 0; i < kBatch; ++i) {
        const auto ask_price2 = static_cast<Price>(200'000 + i);
        for (std::size_t j = 0; j < kFillsPerMatch; ++j) {
          (void)owned.engine->submit_order(next_id++, Side::kAsk, ask_price2,
                                           10);
        }
      }
      state.ResumeTiming();
    }
  }
}
BENCHMARK(BM_SubmitOrderMatch);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
