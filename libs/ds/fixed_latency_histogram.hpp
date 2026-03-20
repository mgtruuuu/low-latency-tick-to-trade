/**
 * @file fixed_latency_histogram.hpp
 * @brief Fixed-bucket linear histogram for latency measurement.
 *
 * Counterpart relationship (planned):
 *   FixedLatencyHistogram : LatencyHistogram  ≈  FixedRingBuffer : RingBuffer
 *   Compile-time capacity → inline std::array storage, constexpr constants.
 *   Runtime capacity → pointer-based storage, runtime constants (future).
 *
 * When to use each:
 *   FixedLatencyHistogram — Known range and bucket count at compile time.
 *     Inline storage keeps buckets on the same cache lines as the owner.
 *   LatencyHistogram (future) — Large bucket counts, config-driven ranges,
 *     or when buckets must live in a specific memory region (huge pages, NUMA).
 *
 * Why a histogram instead of a ring buffer for latency measurement:
 *   FixedRingBuffer keeps the last N raw samples in a sliding window.
 *   Once full, old samples are overwritten and lost — the buffer only reflects
 *   the most recent window, not the full distribution since startup.
 *
 *   A histogram stores counts per bucket, not raw values. Every sample is
 *   reflected in the bucket counts, regardless of how many total samples are
 *   recorded. Memory is O(NumBuckets) regardless of sample volume. The
 *   trade-off is quantization error: percentile values are approximate to
 *   within one BucketWidth.
 *
 * HFT context:
 *   Production latency measurement systems (e.g., HdrHistogram, Gil Tene's
 *   work) universally use bucket-based histograms, not ring buffers, because:
 *   - All samples must be reflected (regulatory and SLA requirements)
 *   - Memory is bounded regardless of uptime (O(buckets), not O(samples))
 *   - Percentile queries are O(buckets) cumulative scan, no sorting needed
 *   - Bucket width determines measurement resolution — 16 cycles ≈ 5ns
 *     at 3GHz, which is sufficient for stage-level instrumentation
 *
 * Design:
 *   - Zero allocation (inline std::array bucket storage)
 *   - O(1) record() on hot path (~2-3ns: one division + one increment)
 *   - Power-of-two BucketWidth → compiler emits right-shift, no division
 *   - Exact min/max tracking (not quantized to bucket boundaries)
 *   - Overflow bucket: values exceeding max range go to the last bucket
 *   - percentile() is cold-path only (linear scan of cumulative counts)
 *   - Single-threaded, noexcept throughout
 *
 * Template parameters:
 *   NumBuckets       — Number of histogram buckets (>= 2)
 *   BucketWidthCycles — Width of each bucket in cycles/units (>= 1).
 *                       Power-of-two values enable shift-based indexing.
 *                       Range covered: [0, NumBuckets * BucketWidthCycles).
 */

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace mk::ds {

template <std::size_t NumBuckets, std::uint64_t BucketWidthCycles>
class FixedLatencyHistogram {
  static_assert(NumBuckets >= 2, "Need at least 2 buckets");
  static_assert(BucketWidthCycles >= 1, "Bucket width must be >= 1");

  // Bucket counters — each bucket covers the half-open range:
  //   [i * BucketWidthCycles, (i+1) * BucketWidthCycles)
  // The last bucket also serves as the overflow bucket for values
  // exceeding (NumBuckets - 1) * BucketWidthCycles.
  std::array<std::uint64_t, NumBuckets> buckets_{};

  std::uint64_t total_count_{0};

  // Exact min/max tracking — not quantized to bucket boundaries.
  // Initialized to sentinel values that will be overwritten on first record().
  std::uint64_t min_{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t max_{0};

public:
  FixedLatencyHistogram() = default;
  ~FixedLatencyHistogram() = default;

  // Non-copyable, non-movable: inline std::array can be large.
  FixedLatencyHistogram(const FixedLatencyHistogram &) = delete;
  FixedLatencyHistogram &operator=(const FixedLatencyHistogram &) = delete;
  FixedLatencyHistogram(FixedLatencyHistogram &&) = delete;
  FixedLatencyHistogram &operator=(FixedLatencyHistogram &&) = delete;

  // ---------------------------------------------------------------------------
  // Modifiers
  // ---------------------------------------------------------------------------

  /// Record a latency sample. O(1), hot path.
  ///
  /// Bucket index = value / BucketWidthCycles, clamped to NumBuckets-1.
  /// When BucketWidthCycles is a power of two, the compiler emits a
  /// right-shift instruction instead of an integer division.
  void record(std::uint64_t value) noexcept {
    auto idx = value / BucketWidthCycles;
    if (idx >= NumBuckets) {
      idx = NumBuckets - 1; // overflow bucket
    }
    ++buckets_[idx];
    ++total_count_;
    min_ = std::min(min_, value);
    max_ = std::max(max_, value);
  }

  /// Reset all counters to zero.
  void clear() noexcept {
    buckets_.fill(0);
    total_count_ = 0;
    min_ = std::numeric_limits<std::uint64_t>::max();
    max_ = 0;
  }

  // ---------------------------------------------------------------------------
  // Observers
  // ---------------------------------------------------------------------------

  /// Total number of samples recorded.
  [[nodiscard]] std::uint64_t total_count() const noexcept {
    return total_count_;
  }

  /// True if no samples have been recorded.
  [[nodiscard]] bool empty() const noexcept { return total_count_ == 0; }

  /// Number of buckets (compile-time constant).
  [[nodiscard]] static constexpr std::size_t num_buckets() noexcept {
    return NumBuckets;
  }

  /// Width of each bucket (compile-time constant).
  [[nodiscard]] static constexpr std::uint64_t bucket_width() noexcept {
    return BucketWidthCycles;
  }

  /// Maximum value covered by the bucket range (exclusive upper bound).
  /// Values at or above this go to the overflow (last) bucket.
  [[nodiscard]] static constexpr std::uint64_t max_range() noexcept {
    return NumBuckets * BucketWidthCycles;
  }

  /// Exact minimum value recorded. Returns UINT64_MAX if empty.
  [[nodiscard]] std::uint64_t min_value() const noexcept { return min_; }

  /// Exact maximum value recorded. Returns 0 if empty.
  [[nodiscard]] std::uint64_t max_value() const noexcept { return max_; }

  /// Count of samples in a specific bucket. Debug-asserts on out-of-range.
  [[nodiscard]] std::uint64_t count_at(std::size_t idx) const noexcept {
    assert(idx < NumBuckets && "Bucket index out of range");
    return buckets_[idx];
  }

  // ---------------------------------------------------------------------------
  // Cold-path percentile query
  // ---------------------------------------------------------------------------

  /// Returns the upper boundary of the bucket containing the p-th percentile.
  ///
  /// Algorithm: scan buckets left-to-right, accumulating counts. When the
  /// cumulative count reaches the target rank (total_count * pct / 100),
  /// that bucket's upper boundary is the percentile value.
  ///
  /// Quantization error: the returned value may differ from the true
  /// percentile by at most BucketWidthCycles. For BucketWidth=16 at 3GHz,
  /// this is ~5.3ns — acceptable for stage-level latency reporting.
  ///
  /// Returns 0 if the histogram is empty.
  [[nodiscard]] std::uint64_t percentile(double pct) const noexcept {
    assert(pct >= 0.0 && pct <= 100.0 && "Percentile must be in [0, 100]");
    if (total_count_ == 0) {
      return 0;
    }

    // Target rank: the number of samples at or below the percentile.
    // Clamp to [1, total_count_] to handle edge cases (pct=0 → rank 1).
    auto target = static_cast<std::uint64_t>(
        pct / 100.0 * static_cast<double>(total_count_));
    if (target == 0) {
      target = 1;
    }

    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < NumBuckets; ++i) {
      cumulative += buckets_[i];
      if (cumulative >= target) {
        // Return the upper boundary of this bucket.
        return (i + 1) * BucketWidthCycles;
      }
    }

    // Should not reach here if total_count_ > 0 and buckets are consistent.
    return NumBuckets * BucketWidthCycles;
  }
};

} // namespace mk::ds
