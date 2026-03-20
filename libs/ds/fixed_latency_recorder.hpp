/**
 * @file fixed_latency_recorder.hpp
 * @brief Fixed-capacity N-stage latency recorder backed by histograms.
 *
 * Wraps N FixedLatencyHistogram instances into a single recorder, indexed
 * by stage number. Designed for multi-stage pipeline latency instrumentation
 * where each stage uses the same histogram configuration.
 *
 * Counterpart relationship (planned):
 *   FixedLatencyRecorder : LatencyRecorder  ≈  FixedHashMap : HashMap
 *   Compile-time capacity → inline storage, constexpr constants.
 *   Runtime capacity → external storage, runtime constants (future).
 *
 * Usage pattern:
 *   enum Stage : std::size_t { kParse = 0, kStrategy = 1, kSend = 2 };
 *   FixedLatencyRecorder<3> recorder;      // 3 stages, default 1024×16
 *   recorder.record(kParse, cycles);       // hot path
 *   auto& h = recorder.histogram(kParse);  // cold path: percentile queries
 *
 * Design:
 *   - Zero allocation (inline std::array of histograms)
 *   - O(1) record() — stage index + histogram record
 *   - Pure data structure — no logging, no I/O dependencies
 *   - Non-copyable, non-movable (NumStages × NumBuckets × 8 bytes)
 *   - Single-threaded, noexcept throughout
 */

#pragma once

#include "ds/fixed_latency_histogram.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace mk::ds {

template <std::size_t NumStages, std::size_t NumBuckets = 1024,
          std::uint64_t BucketWidth = 16>
class FixedLatencyRecorder {
  static_assert(NumStages >= 1, "Need at least 1 stage");

public:
  using Histogram = FixedLatencyHistogram<NumBuckets, BucketWidth>;

  FixedLatencyRecorder() = default;
  ~FixedLatencyRecorder() = default;

  // Non-copyable, non-movable: internal array can be large.
  FixedLatencyRecorder(const FixedLatencyRecorder &) = delete;
  FixedLatencyRecorder &operator=(const FixedLatencyRecorder &) = delete;
  FixedLatencyRecorder(FixedLatencyRecorder &&) = delete;
  FixedLatencyRecorder &operator=(FixedLatencyRecorder &&) = delete;

  // ---------------------------------------------------------------------------
  // Modifiers
  // ---------------------------------------------------------------------------

  /// Record a latency sample for the given stage. O(1), hot path.
  void record(std::size_t stage, std::uint64_t value) noexcept {
    assert(stage < NumStages && "Stage index out of range");
    stages_[stage].record(value);
  }

  /// Reset all stages.
  void clear() noexcept {
    for (auto &s : stages_) {
      s.clear();
    }
  }

  /// Reset a single stage.
  void clear(std::size_t stage) noexcept {
    assert(stage < NumStages && "Stage index out of range");
    stages_[stage].clear();
  }

  // ---------------------------------------------------------------------------
  // Observers
  // ---------------------------------------------------------------------------

  /// Access the histogram for a specific stage (cold path: percentile queries).
  [[nodiscard]] const Histogram &histogram(std::size_t stage) const noexcept {
    assert(stage < NumStages && "Stage index out of range");
    return stages_[stage];
  }

  /// Number of stages (compile-time constant).
  [[nodiscard]] static constexpr std::size_t num_stages() noexcept {
    return NumStages;
  }

  /// Number of buckets per histogram (compile-time constant).
  [[nodiscard]] static constexpr std::size_t num_buckets() noexcept {
    return NumBuckets;
  }

  /// Width of each bucket (compile-time constant).
  [[nodiscard]] static constexpr std::uint64_t bucket_width() noexcept {
    return BucketWidth;
  }

private:
  std::array<Histogram, NumStages> stages_;
};

} // namespace mk::ds
