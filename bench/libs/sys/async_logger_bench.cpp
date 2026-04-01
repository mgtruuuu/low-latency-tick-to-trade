/**
 * @file async_logger_bench.cpp
 * @brief Microbenchmark for async logger enqueue operations.
 *
 * Measures the cost of constructing a LogEntry + pushing it into a SPSC queue
 * via the log_macros.hpp helpers. This is the hot-path cost paid by MD and
 * Strategy threads — the logger thread's drain/format cost is not measured
 * here.
 *
 * Operations benchmarked:
 *   - log_latency   — smallest payload (24 bytes used in union)
 *   - log_order     — largest payload (48 bytes used in union)
 *   - log_market_data — medium payload (32 bytes used in union)
 *
 * Usage:
 *   cmake --build build-reldbg -j$(nproc)
 *   taskset -c 2 ./build-reldbg/bench/libs/sys/async_logger_bench
 */

#include "bench_utils.hpp"
#include "sys/memory/mmap_region.hpp"
#include "sys/memory/spsc_queue.hpp"
#include "sys/nano_clock.hpp"
#include "tick_to_trade/pipeline_log_entry.hpp"
#include "tick_to_trade/pipeline_log_push.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

using namespace mk::sys;
using namespace mk::app;
using namespace mk::sys::memory;
using namespace mk::bench;

namespace {

// ============================================================================
// Configuration
// ============================================================================

constexpr std::size_t kN = 10'000;
constexpr std::size_t kQueueCapacity = 16384; // Large enough to never fill

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// Benchmarks
// ============================================================================

/// Measure log_latency() — construct LogEntry + try_push.
void bench_log_latency(const TscCalibration &cal, PipelineLogQueue &queue) {
  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    (void)log_latency(queue, kThreadIdMd, LatencyStage::kFeedParse,
                      static_cast<std::uint64_t>(i), 0);
  }
  // Drain warm-up entries.
  LogEntry discard{};
  while (queue.try_pop(discard)) {
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    (void)log_latency(queue, kThreadIdMd, LatencyStage::kFeedParse,
                      static_cast<std::uint64_t>(i * 100), 0);
    const auto t1 = rdtsc_end();
    g_latencies[i] = t1 - t0;
  }
  // Drain to prevent queue from filling on repeated runs.
  while (queue.try_pop(discard)) {
  }
  print_stats("log_latency", compute_stats(cal, g_latencies));
}

/// Measure log_order() — largest payload variant.
void bench_log_order(const TscCalibration &cal, PipelineLogQueue &queue) {
  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    (void)log_order(queue, kThreadIdStrategy, LogLevel::kInfo,
                    OrderEvent::kNewOrder, 1, 0, 100000, 10,
                    static_cast<std::uint64_t>(i), 0, 0);
    const auto t1 = rdtsc_end();
    g_latencies[i] = t1 - t0;
  }
  LogEntry discard{};
  while (queue.try_pop(discard)) {
  }
  print_stats("log_order", compute_stats(cal, g_latencies));
}

/// Measure log_market_data() — medium payload variant.
void bench_log_market_data(const TscCalibration &cal, PipelineLogQueue &queue) {
  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    (void)log_market_data(queue, kThreadIdMd, LogLevel::kInfo,
                          static_cast<std::uint64_t>(i), 1, 0, 100000, 50, 0);
    const auto t1 = rdtsc_end();
    g_latencies[i] = t1 - t0;
  }
  LogEntry discard{};
  while (queue.try_pop(discard)) {
  }
  print_stats("log_market_data", compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== AsyncLogger Enqueue Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n", kN);
  std::printf("Queue capacity: %zu\n\n", kQueueCapacity);

  // Allocate SPSC queue buffer via MmapRegion (matches pipeline usage).
  const auto buf_size =
      SPSCQueue<LogEntry>::required_buffer_size(kQueueCapacity);
  auto region = MmapRegion::allocate_anonymous(buf_size);
  if (!region) {
    std::printf("ERROR: mmap failed\n");
    return 1;
  }

  auto queue = SPSCQueue<LogEntry>::create(region->get(), region->size(),
                                           kQueueCapacity);
  if (!queue) {
    std::printf("ERROR: queue creation failed\n");
    return 1;
  }

  print_header();
  bench_log_latency(cal, *queue);
  bench_log_order(cal, *queue);
  bench_log_market_data(cal, *queue);

  std::printf("\nTip: taskset -c N ./async_logger_bench  for stable p99.\n");

  return 0;
}
