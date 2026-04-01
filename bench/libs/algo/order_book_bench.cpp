/**
 * @file order_book_bench.cpp
 * @brief Microbenchmark for OrderBook operations.
 *
 * Measures per-operation latency using serialized rdtsc (CPU cycle counter).
 * Reports min, median, p99, and max in nanoseconds.
 *
 * No external framework — rdtsc_start()/rdtsc_end() + manual percentile
 * computation. This is itself an interview talking point: "I benchmarked
 * my own data structure using serialized rdtsc with TSC frequency
 * calibration."
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/libs/algo/order_book_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/libs/algo/order_book_bench
 */

#include "algo/order_book.hpp"
#include "bench_utils.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace mk::algo;
using namespace mk::sys;
using namespace mk::bench;

namespace {

// ============================================================================
// Benchmark configuration
// ============================================================================

// Capacity large enough for 10K operations per benchmark.
// Total memory: ~1.5MB (order pool + level pool + hash maps) — fits in L3.
constexpr OrderBook::Params kBenchParams{
    .max_orders = 16384,
    .max_levels = 1024,
    .order_map_cap = 16384,
    .level_map_cap = 2048,
};

constexpr std::size_t kN = 10'000;

// Global latency buffer. 10K × 8 bytes = 80KB.
std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// Benchmarks
// ============================================================================

// --- add_order (existing level) ---
//
// The price level already exists — measures the common case:
//   hash lookup (dup check) + pool pop + hash insert + list push_back.
// No sorted insertion cost.
void bench_add_existing_level(const TscCalibration &cal) {
  std::vector<std::byte> buf(OrderBook::required_buffer_size(kBenchParams));
  OrderBook book(buf.data(), buf.size(), kBenchParams);

  // Create one level at price 100.
  (void)book.add_order(1, Side::kBid, 100, 10);

  // Warm-up: cycle add/cancel to prime caches.
  for (std::size_t i = 0; i < 500; ++i) {
    (void)book.add_order(static_cast<OrderId>(1000 + i), Side::kBid, 100, 10);
  }
  for (std::size_t i = 0; i < 500; ++i) {
    (void)book.cancel_order(static_cast<OrderId>(1000 + i));
  }

  // Measure.
  for (std::size_t i = 0; i < kN; ++i) {
    const auto id = static_cast<OrderId>(10'000 + i);
    const auto t0 = rdtsc_start();
    (void)book.add_order(id, Side::kBid, 100, 10);
    const auto t1 = rdtsc_end();
    g_latencies[i] = t1 - t0;
  }

  print_stats("add_order (existing level)", compute_stats(cal, g_latencies));
}

// --- add_order (new level) ---
//
// Each order creates a new price level. Measures the full path:
//   above + alloc_level + level hash insert + sorted insertion.
// Bid prices ascend → each is the new best → front insertion (k=1).
void bench_add_new_level(const TscCalibration &cal) {
  // MaxLevels = 1024, so we batch: add ~900 levels, clear, repeat.
  constexpr std::size_t kBatch = 900;
  std::size_t measured = 0;

  std::vector<std::byte> buf(OrderBook::required_buffer_size(kBenchParams));
  OrderBook book(buf.data(), buf.size(), kBenchParams);

  while (measured < kN) {
    const std::size_t count = std::min(kBatch, kN - measured);

    for (std::size_t i = 0; i < count; ++i) {
      const auto id = static_cast<OrderId>(measured + i + 1);
      // Ascending bid prices → each is new best, inserted at front.
      const auto price = static_cast<Price>(10'000 + measured + i);

      const auto t0 = rdtsc_start();
      (void)book.add_order(id, Side::kBid, price, 10);
      const auto t1 = rdtsc_end();
      g_latencies[measured + i] = t1 - t0;
    }
    measured += count;
    book.clear();
  }

  print_stats("add_order (new level)", compute_stats(cal, g_latencies));
}

// --- cancel_order ---
//
// Measures the most latency-sensitive operation:
//   hash lookup + back-pointer + intrusive unlink + hash erase + pool push.
// Orders spread across 50 price levels for realistic hash distribution.
// Cancelled in reverse order (most recent first) — realistic: market makers
// cancel their newest quote when updating.
void bench_cancel(const TscCalibration &cal) {
  std::vector<std::byte> buf(OrderBook::required_buffer_size(kBenchParams));
  OrderBook book(buf.data(), buf.size(), kBenchParams);

  constexpr std::size_t kLevels = 50;
  for (std::size_t i = 0; i < kN; ++i) {
    const auto price = static_cast<Price>(100 + (i % kLevels));
    (void)book.add_order(static_cast<OrderId>(i + 1), Side::kBid, price, 10);
  }

  // Measure: cancel in reverse order (most recent first).
  for (std::size_t i = 0; i < kN; ++i) {
    const auto id = static_cast<OrderId>(kN - i);
    const auto t0 = rdtsc_start();
    (void)book.cancel_order(id);
    const auto t1 = rdtsc_end();
    g_latencies[i] = t1 - t0;
  }

  print_stats("cancel_order", compute_stats(cal, g_latencies));
}

// --- modify_order (qty reduction) ---
//
// Simplest hot-path operation:
//   hash lookup + field update + level aggregate update.
// No list manipulation, no level creation/destruction.
void bench_modify(const TscCalibration &cal) {
  std::vector<std::byte> buf(OrderBook::required_buffer_size(kBenchParams));
  OrderBook book(buf.data(), buf.size(), kBenchParams);

  constexpr std::size_t kLevels = 50;
  for (std::size_t i = 0; i < kN; ++i) {
    const auto price = static_cast<Price>(100 + (i % kLevels));
    (void)book.add_order(static_cast<OrderId>(i + 1), Side::kBid, price, 100);
  }

  // Measure.
  for (std::size_t i = 0; i < kN; ++i) {
    const auto id = static_cast<OrderId>(i + 1);
    const auto t0 = rdtsc_start();
    (void)book.modify_order(id, 50);
    const auto t1 = rdtsc_end();
    g_latencies[i] = t1 - t0;
  }

  print_stats("modify_order (qty reduction)", compute_stats(cal, g_latencies));
}

// --- best_bid ---
//
// Single pointer dereference (front of intrusive list).
// The fastest operation — baseline for comparison.
void bench_best_bid(const TscCalibration &cal) {
  std::vector<std::byte> buf(OrderBook::required_buffer_size(kBenchParams));
  OrderBook book(buf.data(), buf.size(), kBenchParams);

  for (std::size_t i = 0; i < 100; ++i) {
    (void)book.add_order(static_cast<OrderId>(i + 1), Side::kBid,
                         static_cast<Price>(100 + i), 10);
  }

  // Accumulate results to prevent the compiler from hoisting best_bid()
  // out of the loop (it's a pure read — no side effects).
  Price sum = 0;
  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    const auto bid = book.best_bid();
    const auto t1 = rdtsc_end();
    sum += bid.value_or(0);
    g_latencies[i] = t1 - t0;
  }
  do_not_optimize(sum);

  print_stats("best_bid", compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== OrderBook Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n\n", kN);

  print_header();
  bench_add_existing_level(cal);
  bench_add_new_level(cal);
  bench_cancel(cal);
  bench_modify(cal);
  bench_best_bid(cal);

  std::printf("\nTip: taskset -c N ./order_book_bench  for stable p99.\n");

  return 0;
}
