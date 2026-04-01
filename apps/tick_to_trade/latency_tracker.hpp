/**
 * @file latency_tracker.hpp
 * @brief RDTSC-based per-stage latency instrumentation.
 *
 * Records raw TSC cycle counts at each stage boundary of the tick-to-trade
 * pipeline. On shutdown, converts cycles to nanoseconds via TscCalibration
 * and reports percentile statistics (min, median, p99, p999, max).
 *
 * HFT context:
 *   Latency measurement is not optional in HFT — it IS the product.
 *   Every firm measures tick-to-trade latency at sub-microsecond precision.
 *   Key practices:
 *   - Use RDTSC (not clock_gettime) for ~1ns measurement overhead
 *   - Report percentiles (p50/p99/p999), not averages
 *   - Measure each stage independently to identify bottlenecks
 *   - Pin the measurement thread to a core to avoid TSC drift
 *
 *   This tracker uses plain rdtsc() (not rdtsc_start/end) on the hot path
 *   to minimize measurement perturbation. The serializing variants add
 *   ~30-40 cycles of pipeline drain overhead per call.
 *
 * Design:
 *   - Zero allocation (FixedLatencyHistogram for per-stage measurement).
 *   - Histogram preserves ALL samples as bucket counts — no data loss
 *     regardless of uptime (unlike ring buffers that overwrite old data).
 *   - Inline record() on hot path (~3ns overhead per call).
 *   - print_stats() is cold-path only (cumulative scan for percentiles).
 *
 * Structure:
 *   Tick-to-trade and stage breakdown are separate:
 *   - tick_to_trade_: single histogram for the full pipeline latency
 *   - stages_: FixedLatencyRecorder with 4 diagnostic stages
 *   - order_rtt_: separate histogram (nanoseconds, different bucket width)
 */

#pragma once

#include "ds/fixed_latency_histogram.hpp"
#include "ds/fixed_latency_recorder.hpp"
#include "sys/log/signal_logger.hpp"
#include "sys/nano_clock.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace mk::app {

class LatencyTracker {
public:
  // ---------------------------------------------------------------------------
  // Tick-to-trade measurement (full pipeline)
  // ---------------------------------------------------------------------------

  /// Record a tick-to-trade measurement (UDP recv to TCP send).
  void record_tick_to_trade(std::uint64_t start_tsc,
                            std::uint64_t end_tsc) noexcept {
    if (end_tsc > start_tsc) {
      const auto cycles = end_tsc - start_tsc;
      tick_to_trade_.record(cycles);
      total_ticks_.fetch_add(1, std::memory_order_relaxed);
      if (warmup_t2t_count_ < kWarmupTicks) [[unlikely]] {
        warmup_tick_to_trade_[warmup_t2t_count_++] = cycles;
      }
    }
  }

  // ---------------------------------------------------------------------------
  // Per-stage breakdown
  // ---------------------------------------------------------------------------

  /// Record per-stage breakdown: feed parse latency.
  void record_feed_parse(std::uint64_t cycles) noexcept {
    stages_.record(Stage::kFeedParse, cycles);
    if (warmup_parse_count_ < kWarmupTicks) [[unlikely]] {
      warmup_feed_parse_[warmup_parse_count_++] = cycles;
    }
  }

  /// Record SPSC queue hop latency (td - t0).
  /// Measured from MD thread's rdtsc at recv to Strategy thread's rdtsc
  /// at per-item processing. Includes batch-internal processing accumulation:
  /// later items in a drain batch include earlier items' processing time.
  void record_queue_latency(std::uint64_t cycles) noexcept {
    stages_.record(Stage::kQueueHop, cycles);
    if (warmup_queue_count_ < kWarmupTicks) [[unlikely]] {
      warmup_queue_hop_[warmup_queue_count_++] = cycles;
    }
  }

  /// Record pure queue wait latency (t_drain - t0).
  /// Measured from MD thread's rdtsc at recv to Strategy thread's rdtsc
  /// immediately after drain(). This excludes batch-internal processing
  /// accumulation — isolates how long data actually waited in the queue.
  void record_queue_wait(std::uint64_t cycles) noexcept {
    queue_wait_.record(cycles);
  }

  /// Record per-stage breakdown: strategy evaluation latency.
  void record_strategy(std::uint64_t cycles) noexcept {
    stages_.record(Stage::kStrategy, cycles);
    if (warmup_strat_count_ < kWarmupTicks) [[unlikely]] {
      warmup_strategy_[warmup_strat_count_++] = cycles;
    }
  }

  /// Record per-stage breakdown: order serialize + send latency.
  void record_order_send(std::uint64_t cycles) noexcept {
    stages_.record(Stage::kOrderSend, cycles);
    if (warmup_send_count_ < kWarmupTicks) [[unlikely]] {
      warmup_order_send_[warmup_send_count_++] = cycles;
    }
  }

  // ---------------------------------------------------------------------------
  // Order round-trip (external, nanosecond-based)
  // ---------------------------------------------------------------------------

  /// Record order round-trip (send timestamp to ack/fill receipt).
  void record_order_rtt(std::int64_t send_ts_nanos) noexcept {
    auto now = sys::monotonic_nanos();
    if (now > send_ts_nanos) {
      order_rtt_.record(static_cast<std::uint64_t>(now - send_ts_nanos));
      total_rtts_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // ---------------------------------------------------------------------------
  // Reporting (cold path)
  // ---------------------------------------------------------------------------

  /// Print statistics on shutdown (cold path).
  /// Converts TSC cycles to nanoseconds using the calibration data.
  /// Order RTT is already in nanoseconds (uses monotonic_nanos).
  void print_stats(const sys::TscCalibration &cal) noexcept {
    sys::log::signal_log(
        "\n====================================================\n");
    sys::log::signal_log("         LATENCY REPORT\n");
    sys::log::signal_log(
        "====================================================\n");

    char line[256];

    // TSC frequency info.
    std::snprintf(line, sizeof(line), "  TSC freq: %.3f GHz\n\n",
                  cal.freq_ghz());
    sys::log::signal_log(line);

    // Tick-to-trade (TSC cycles → nanoseconds).
    print_stage_stats("Tick-to-Trade", tick_to_trade_, cal, true);
    sys::log::signal_log("  Total ticks processed: ",
                         total_ticks_.load(std::memory_order_relaxed), '\n');
    sys::log::signal_log('\n');

    // Per-stage breakdown (TSC cycles → nanoseconds).
    sys::log::signal_log("--- Stage Breakdown ---\n");
    print_stage_stats("Feed Parse", stages_.histogram(Stage::kFeedParse), cal,
                      true);
    print_stage_stats("Queue Hop", stages_.histogram(Stage::kQueueHop), cal,
                      true);
    print_stage_stats("Queue Wait", queue_wait_, cal, true);
    print_stage_stats("Strategy", stages_.histogram(Stage::kStrategy), cal,
                      true);
    print_stage_stats("Order Send", stages_.histogram(Stage::kOrderSend), cal,
                      true);
    sys::log::signal_log('\n');

    // Order RTT (already in nanoseconds).
    print_stage_stats("Order RTT", order_rtt_, cal, false);
    sys::log::signal_log("  Total RTT samples: ",
                         total_rtts_.load(std::memory_order_relaxed), '\n');

    sys::log::signal_log(
        "====================================================\n\n");

    print_warmup_analysis(cal);
  }

  // ---------------------------------------------------------------------------
  // Observers
  // ---------------------------------------------------------------------------

  [[nodiscard]] std::uint64_t total_ticks() const noexcept {
    return total_ticks_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t total_rtts() const noexcept {
    return total_rtts_.load(std::memory_order_relaxed);
  }

private:
  static constexpr std::uint64_t kWarmupTicks = 256;

  // -- Histogram parameters ---------------------------------------------------

  // TSC cycle stages: 1024 buckets × 128 cycles = range [0, 131072 cycles).
  // At 2.4GHz: 131072 cycles ≈ 54us. Covers Order Send + TCP syscall latency.
  // Bucket resolution: 128 cycles ≈ 53ns at 2.4GHz.
  // Power-of-two width enables right-shift indexing (no division).
  static constexpr std::size_t kTscNumBuckets = 1024;
  static constexpr std::uint64_t kTscBucketWidth = 128;

  // Order RTT is in nanoseconds (from monotonic_nanos), not TSC cycles.
  // 4096 buckets × 1000ns = range [0, 4096000ns ≈ 4ms).
  // Covers typical two-machine RTT (~400µs–2ms). Resolution: 1µs.
  static constexpr std::size_t kRttNumBuckets = 4096;
  static constexpr std::uint64_t kRttBucketWidth = 1000;

  // -- Stage breakdown indices ------------------------------------------------

  // Diagnostic per-stage breakdown. Separate from end-to-end measurement.
  enum Stage : std::size_t {
    kFeedParse = 0,
    kQueueHop,
    kStrategy,
    kOrderSend,
    kNumStages
  };

  // -- Histograms -------------------------------------------------------------

  // Tick-to-trade: full pipeline latency (UDP recv → TCP send).
  ds::FixedLatencyHistogram<kTscNumBuckets, kTscBucketWidth> tick_to_trade_;

  // Per-stage breakdown: 4 diagnostic stages.
  ds::FixedLatencyRecorder<kNumStages, kTscNumBuckets, kTscBucketWidth> stages_;

  // Pure queue wait: t_drain - t0 (excludes batch-internal processing).
  ds::FixedLatencyHistogram<kTscNumBuckets, kTscBucketWidth> queue_wait_;

  // Order RTT: different bucket width → separate histogram.
  ds::FixedLatencyHistogram<kRttNumBuckets, kRttBucketWidth> order_rtt_;

  // -- Diagnostic counters ----------------------------------------------------

  // std::atomic for cross-thread monitoring safety.
  // On x86-64, relaxed atomic store compiles to a plain MOV (zero overhead).
  std::atomic<std::uint64_t> total_ticks_{0};
  std::atomic<std::uint64_t> total_rtts_{0};

  // -- Warm-up data -----------------------------------------------------------

  // First kWarmupTicks samples stored separately per stage.
  // After kWarmupTicks, recording stops here; main histograms continue
  // collecting steady-state data. Write-once arrays — no overwrite needed.
  std::array<std::uint64_t, kWarmupTicks> warmup_tick_to_trade_{};
  std::array<std::uint64_t, kWarmupTicks> warmup_feed_parse_{};
  std::array<std::uint64_t, kWarmupTicks> warmup_queue_hop_{};
  std::array<std::uint64_t, kWarmupTicks> warmup_strategy_{};
  std::array<std::uint64_t, kWarmupTicks> warmup_order_send_{};
  std::uint64_t warmup_t2t_count_{0};
  std::uint64_t warmup_parse_count_{0};
  std::uint64_t warmup_queue_count_{0};
  std::uint64_t warmup_strat_count_{0};
  std::uint64_t warmup_send_count_{0};

  // -- Reporting helpers (cold path) ------------------------------------------

  /// Print percentile stats for one stage using histogram data.
  /// @param use_tsc If true, convert cycle values to ns via cal. If false,
  ///        values are already in nanoseconds.
  template <std::size_t N, std::uint64_t W>
  static void print_stage_stats(const char *name,
                                const ds::FixedLatencyHistogram<N, W> &hist,
                                const sys::TscCalibration &cal,
                                bool use_tsc) noexcept {

    if (hist.empty()) {
      sys::log::signal_log("  ", name, ": (no samples)\n");
      return;
    }

    auto to_ns = [&](std::uint64_t val) -> double {
      if (use_tsc) {
        return cal.to_ns(val);
      }
      return static_cast<double>(val);
    };

    char line[256];
    std::snprintf(line, sizeof(line),
                  "  %-14s  min=%7.0f  p50=%7.0f  p99=%7.0f  "
                  "p999=%7.0f  max=%7.0f ns  (n=%llu)\n",
                  name, to_ns(hist.min_value()), to_ns(hist.percentile(50.0)),
                  to_ns(hist.percentile(99.0)), to_ns(hist.percentile(99.9)),
                  to_ns(hist.max_value()),
                  static_cast<unsigned long long>(hist.total_count()));
    sys::log::signal_log(line);
  }

  /// Print sorted percentile stats for a warm-up array.
  static void print_warmup_stage(const char *name,
                                 std::array<std::uint64_t, kWarmupTicks> &data,
                                 std::uint64_t count,
                                 const sys::TscCalibration &cal) noexcept {
    if (count == 0) {
      return;
    }
    std::sort(data.begin(), data.begin() + count);

    auto percentile = [&](double pct) -> double {
      auto idx = static_cast<std::size_t>(pct / 100.0 *
                                          static_cast<double>(count - 1));
      return cal.to_ns(data[idx]);
    };

    char line[256];
    std::snprintf(line, sizeof(line),
                  "  %-14s  min=%7.0f  p50=%7.0f  p99=%7.0f  "
                  "max=%7.0f ns  (n=%llu)\n",
                  name, cal.to_ns(data[0]), percentile(50.0), percentile(99.0),
                  cal.to_ns(data[count - 1]),
                  static_cast<unsigned long long>(count));
    sys::log::signal_log(line);
  }

  /// Print warm-up vs steady-state comparison (cold path, called from
  /// print_stats). Requires enough data in both phases.
  void print_warmup_analysis(const sys::TscCalibration &cal) noexcept {
    // Need at least kWarmupTicks feed parse samples to have captured a
    // full warm-up window, plus enough total samples for the main
    // histograms to reflect steady-state data.
    if (warmup_parse_count_ < kWarmupTicks) {
      return;
    }

    sys::log::signal_log("--- Warm-Up Analysis (first ", kWarmupTicks,
                         " updates) ---\n");
    if (warmup_t2t_count_ > 0) {
      print_warmup_stage("Tick-to-Trade", warmup_tick_to_trade_,
                         warmup_t2t_count_, cal);
    }
    print_warmup_stage("Feed Parse", warmup_feed_parse_, warmup_parse_count_,
                       cal);
    print_warmup_stage("Queue Hop", warmup_queue_hop_, warmup_queue_count_,
                       cal);
    print_warmup_stage("Strategy", warmup_strategy_, warmup_strat_count_, cal);
    print_warmup_stage("Order Send", warmup_order_send_, warmup_send_count_,
                       cal);
    sys::log::signal_log('\n');
  }
};

} // namespace mk::app
