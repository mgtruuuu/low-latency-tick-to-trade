/**
 * @file order_book_gbench.cpp
 * @brief Google Benchmark version of OrderBook benchmarks.
 *
 * Companion to order_book_bench.cpp (custom rdtsc). This version uses Google
 * Benchmark for CI-friendly output, regression tracking, and JSON export.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/algo/order_book_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/algo/order_book_gbench --benchmark_format=json
 *
 * Filter specific benchmarks:
 *   ./build/bench/algo/order_book_gbench --benchmark_filter=BM_CancelOrder
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/algo/order_book_gbench
 */

#include "algo/order_book.hpp"

#include <cstddef>
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

// ============================================================================
// Benchmarks
// ============================================================================

// --- add_order (existing level) ---
//
// The price level already exists — measures the common case:
//   hash lookup (dup check) + pool pop + hash insert + list push_back.
//
// Batched: clear the book every 15000 orders to stay within MaxOrders (16384).
// Without this, pool exhaustion causes add_order to return false, and the
// benchmark would measure the fast failure path instead of real insertions.
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_AddOrderExistingLevel(benchmark::State &state) {
  std::vector<std::byte> buf(OrderBook::required_buffer_size(kBenchParams));
  OrderBook book(buf.data(), buf.size(), kBenchParams);

  // Create one level at price 100.
  (void)book.add_order(1, Side::kBid, 100, 10);

  std::size_t i = 0;
  for (auto _ : state) {
    if (i > 0 && i % 15'000 == 0) {
      state.PauseTiming();
      book.clear();
      (void)book.add_order(1, Side::kBid, 100, 10);
      state.ResumeTiming();
    }
    const auto id = static_cast<OrderId>(i + 1000);
    (void)book.add_order(id, Side::kBid, 100, 10);
    ++i;
  }
}
BENCHMARK(BM_AddOrderExistingLevel);

// --- add_order (new level) ---
//
// Each order creates a new price level. Measures the full path:
//   above + alloc_level + level hash insert + sorted insertion.
// Bid prices ascend → each is the new best → front insertion (k=1).
//
// Batched: clear the book every 900 orders to stay within MaxLevels.
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_AddOrderNewLevel(benchmark::State &state) {
  std::vector<std::byte> buf(OrderBook::required_buffer_size(kBenchParams));
  OrderBook book(buf.data(), buf.size(), kBenchParams);
  std::size_t i = 0;

  for (auto _ : state) {
    if (i > 0 && i % 900 == 0) {
      state.PauseTiming();
      book.clear();
      state.ResumeTiming();
    }
    const auto id = static_cast<OrderId>(i + 1);
    const auto price = static_cast<Price>(10'000 + i);
    (void)book.add_order(id, Side::kBid, price, 10);
    ++i;
  }
}
BENCHMARK(BM_AddOrderNewLevel);

// --- cancel_order ---
//
// Hash lookup + back-pointer + intrusive unlink + hash erase + pool push.
// Orders spread across 50 price levels for realistic hash distribution.
// Cancelled in reverse order (most recent first).
//
// Book lives outside the loop — constructed/destroyed once (cold path).
// Each iteration: clear + repopulate (paused) → cancel all (timed).
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_CancelOrder(benchmark::State &state) {
  constexpr std::size_t kOrders = 10'000;
  constexpr std::size_t kLevels = 50;

  std::vector<std::byte> buf(OrderBook::required_buffer_size(kBenchParams));
  OrderBook book(buf.data(), buf.size(), kBenchParams);

  for (auto _ : state) {
    state.PauseTiming();
    book.clear();
    for (std::size_t i = 0; i < kOrders; ++i) {
      const auto price = static_cast<Price>(100 + (i % kLevels));
      (void)book.add_order(static_cast<OrderId>(i + 1), Side::kBid, price, 10);
    }
    state.ResumeTiming();

    for (std::size_t i = 0; i < kOrders; ++i) {
      const auto id = static_cast<OrderId>(kOrders - i);
      (void)book.cancel_order(id);
    }
  }

  // Report per-operation time (Google Benchmark reports total by default).
  // SetItemsProcessed takes the TOTAL across all iterations; the framework
  // divides by duration to produce items/s.
  state.SetItemsProcessed(
      static_cast<std::int64_t>(state.iterations() * kOrders));
}
BENCHMARK(BM_CancelOrder);

// --- modify_order (qty reduction) ---
//
// Simplest hot-path operation:
//   hash lookup + field update + level aggregate update.
//
// Batched: rebuild the book every kOrders iterations. modify_order only
// allows reduction (new_qty < current_qty) — after one full pass all
// orders are at qty 50, so subsequent modify_order(id, 50) returns false
// immediately (new_qty >= current_qty). Resetting ensures every call
// performs a real modification.
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_ModifyOrder(benchmark::State &state) {
  constexpr std::size_t kOrders = 10'000;
  constexpr std::size_t kLevels = 50;

  std::vector<std::byte> buf(OrderBook::required_buffer_size(kBenchParams));
  OrderBook book(buf.data(), buf.size(), kBenchParams);
  for (std::size_t i = 0; i < kOrders; ++i) {
    const auto price = static_cast<Price>(100 + (i % kLevels));
    (void)book.add_order(static_cast<OrderId>(i + 1), Side::kBid, price, 100);
  }

  std::size_t idx = 0;
  for (auto _ : state) {
    if (idx > 0 && idx % kOrders == 0) {
      state.PauseTiming();
      book.clear();
      for (std::size_t i = 0; i < kOrders; ++i) {
        const auto price = static_cast<Price>(100 + (i % kLevels));
        (void)book.add_order(static_cast<OrderId>(i + 1), Side::kBid, price,
                             100);
      }
      state.ResumeTiming();
    }
    const auto id = static_cast<OrderId>((idx % kOrders) + 1);
    (void)book.modify_order(id, 50);
    ++idx;
  }
}
BENCHMARK(BM_ModifyOrder);

// --- best_bid ---
//
// Single pointer dereference (front of intrusive list).
// The fastest operation — baseline for comparison.
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_BestBid(benchmark::State &state) {
  std::vector<std::byte> buf(OrderBook::required_buffer_size(kBenchParams));
  OrderBook book(buf.data(), buf.size(), kBenchParams);

  for (std::size_t i = 0; i < 100; ++i) {
    (void)book.add_order(static_cast<OrderId>(i + 1), Side::kBid,
                         static_cast<Price>(100 + i), 10);
  }

  Price sum = 0;
  for (auto _ : state) {
    const auto bid = book.best_bid();
    sum += bid.value_or(0);
    benchmark::DoNotOptimize(sum);
  }
}
BENCHMARK(BM_BestBid);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
