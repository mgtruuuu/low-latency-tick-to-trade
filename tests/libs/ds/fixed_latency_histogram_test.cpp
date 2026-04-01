/**
 * @file fixed_latency_histogram_test.cpp
 * @brief Tests for mk::ds::FixedLatencyHistogram — fixed-bucket histogram.
 */

#include "ds/fixed_latency_histogram.hpp"

#include <gtest/gtest.h>

namespace {

// Convenient alias — BucketWidth=10 makes manual bucket math easy.
using Hist = mk::ds::FixedLatencyHistogram<16, 10>;

// =============================================================================
// 1. EmptyOnConstruction
// =============================================================================

TEST(FixedLatencyHistogramTest, EmptyOnConstruction) {
  const Hist h;
  EXPECT_TRUE(h.empty());
  EXPECT_EQ(h.total_count(), 0U);
  EXPECT_EQ(h.num_buckets(), 16U);
  EXPECT_EQ(h.bucket_width(), 10U);
  EXPECT_EQ(h.max_range(), 160U);
  EXPECT_EQ(h.percentile(50.0), 0U);
  EXPECT_EQ(h.percentile(99.0), 0U);
}

// =============================================================================
// 2. RecordSingleValue
// =============================================================================

TEST(FixedLatencyHistogramTest, RecordSingleValue) {
  Hist h;
  h.record(42);

  EXPECT_FALSE(h.empty());
  EXPECT_EQ(h.total_count(), 1U);
  EXPECT_EQ(h.min_value(), 42U);
  EXPECT_EQ(h.max_value(), 42U);

  // Value 42 → bucket 42/10 = 4 → upper boundary = 50.
  EXPECT_EQ(h.percentile(50.0), 50U);
  EXPECT_EQ(h.percentile(99.0), 50U);
}

// =============================================================================
// 3. BucketIndexing
// =============================================================================

TEST(FixedLatencyHistogramTest, BucketIndexing) {
  Hist h;

  // Bucket 0: [0, 10)
  h.record(0);
  h.record(5);
  h.record(9);
  EXPECT_EQ(h.count_at(0), 3U);

  // Bucket 1: [10, 20)
  h.record(10);
  h.record(15);
  EXPECT_EQ(h.count_at(1), 2U);

  // Bucket 2: [20, 30)
  h.record(25);
  EXPECT_EQ(h.count_at(2), 1U);

  // Other buckets remain zero.
  EXPECT_EQ(h.count_at(3), 0U);
  EXPECT_EQ(h.count_at(15), 0U);

  EXPECT_EQ(h.total_count(), 6U);
}

// =============================================================================
// 4. OverflowBucket
// =============================================================================

TEST(FixedLatencyHistogramTest, OverflowBucket) {
  Hist h; // 16 buckets × 10 width = range [0, 160)

  // Values at or above max_range go to the last bucket (index 15).
  h.record(160);
  h.record(500);
  h.record(999);

  EXPECT_EQ(h.count_at(15), 3U);
  EXPECT_EQ(h.total_count(), 3U);
  EXPECT_EQ(h.min_value(), 160U);
  EXPECT_EQ(h.max_value(), 999U);
}

// =============================================================================
// 5. BoundaryValues
// =============================================================================

TEST(FixedLatencyHistogramTest, BoundaryValues) {
  Hist h;

  // Exact bucket boundary: 10 → bucket 1 (10/10 = 1).
  h.record(10);
  EXPECT_EQ(h.count_at(1), 1U);

  // Just below boundary: 9 → bucket 0 (9/10 = 0).
  h.record(9);
  EXPECT_EQ(h.count_at(0), 1U);

  // Last regular bucket boundary: 150 → bucket 15 (150/10 = 15).
  h.record(150);
  EXPECT_EQ(h.count_at(15), 1U);
}

// =============================================================================
// 6. Percentiles
// =============================================================================

TEST(FixedLatencyHistogramTest, Percentiles) {
  Hist h;

  // Put 100 samples in bucket 0 [0,10), 100 in bucket 5 [50,60).
  for (int i = 0; i < 100; ++i) {
    h.record(5); // bucket 0
  }
  for (int i = 0; i < 100; ++i) {
    h.record(55); // bucket 5
  }

  EXPECT_EQ(h.total_count(), 200U);

  // p50 = 50th percentile. Target rank = 100.
  // Bucket 0 has 100 samples → cumulative reaches 100 at bucket 0.
  // Upper boundary of bucket 0 = 10.
  EXPECT_EQ(h.percentile(50.0), 10U);

  // p51 → target rank = 102. Bucket 0 has 100, need 2 more → bucket 5.
  // Upper boundary of bucket 5 = 60.
  EXPECT_EQ(h.percentile(51.0), 60U);

  // p99 → target rank = 198. Still in bucket 5 (cumulative = 200).
  EXPECT_EQ(h.percentile(99.0), 60U);

  // p100 → target rank = 200. Still in bucket 5.
  EXPECT_EQ(h.percentile(100.0), 60U);
}

// =============================================================================
// 7. PercentileUniformDistribution
// =============================================================================

TEST(FixedLatencyHistogramTest, PercentileUniformDistribution) {
  // 8 buckets × 100 width = range [0, 800).
  mk::ds::FixedLatencyHistogram<8, 100> h;

  // 100 samples in each bucket (uniform distribution).
  for (int b = 0; b < 8; ++b) {
    for (int i = 0; i < 100; ++i) {
      h.record((static_cast<std::uint64_t>(b) * 100) + 50); // mid-bucket
    }
  }
  ASSERT_EQ(h.total_count(), 800U);

  // p50 → target rank = 400. Buckets 0-3 have 400 samples.
  // Upper boundary of bucket 3 = 400.
  EXPECT_EQ(h.percentile(50.0), 400U);

  // p25 → target rank = 200. Buckets 0-1 have 200.
  // Upper boundary of bucket 1 = 200.
  EXPECT_EQ(h.percentile(25.0), 200U);

  // p75 → target rank = 600. Buckets 0-5 have 600.
  // Upper boundary of bucket 5 = 600.
  EXPECT_EQ(h.percentile(75.0), 600U);
}

// =============================================================================
// 8. MinMaxExact
// =============================================================================

TEST(FixedLatencyHistogramTest, MinMaxExact) {
  Hist h;

  h.record(42);
  EXPECT_EQ(h.min_value(), 42U);
  EXPECT_EQ(h.max_value(), 42U);

  h.record(7);
  EXPECT_EQ(h.min_value(), 7U);
  EXPECT_EQ(h.max_value(), 42U);

  h.record(100);
  EXPECT_EQ(h.min_value(), 7U);
  EXPECT_EQ(h.max_value(), 100U);

  // Overflow value — max tracks exact value, not bucket boundary.
  h.record(999);
  EXPECT_EQ(h.max_value(), 999U);
}

// =============================================================================
// 9. ClearResetsEverything
// =============================================================================

TEST(FixedLatencyHistogramTest, ClearResetsEverything) {
  Hist h;

  for (int i = 0; i < 100; ++i) {
    h.record(static_cast<std::uint64_t>(i));
  }
  ASSERT_EQ(h.total_count(), 100U);

  h.clear();

  EXPECT_TRUE(h.empty());
  EXPECT_EQ(h.total_count(), 0U);
  EXPECT_EQ(h.percentile(50.0), 0U);

  // All buckets should be zero.
  for (std::size_t i = 0; i < Hist::num_buckets(); ++i) {
    EXPECT_EQ(h.count_at(i), 0U);
  }

  // Record after clear works correctly.
  h.record(50);
  EXPECT_EQ(h.total_count(), 1U);
  EXPECT_EQ(h.min_value(), 50U);
  EXPECT_EQ(h.max_value(), 50U);
}

// =============================================================================
// 10. LargeVolume
// =============================================================================

TEST(FixedLatencyHistogramTest, LargeVolume) {
  // 1024 buckets × 16 cycles = range [0, 16384). Matches LatencyTracker usage.
  mk::ds::FixedLatencyHistogram<1024, 16> h;

  constexpr std::uint64_t kSamples = 1'000'000;
  for (std::uint64_t i = 0; i < kSamples; ++i) {
    // Simulate latency values in [100, 5000) cycles.
    h.record(100 + (i % 4900));
  }

  EXPECT_EQ(h.total_count(), kSamples);
  EXPECT_EQ(h.min_value(), 100U);
  EXPECT_EQ(h.max_value(), 4999U);

  // Percentiles should be within the recorded range.
  EXPECT_GT(h.percentile(50.0), 0U);
  EXPECT_LE(h.percentile(50.0), 5000U);
  EXPECT_LE(h.percentile(50.0), h.percentile(99.0));
  EXPECT_LE(h.percentile(99.0), h.percentile(99.9));
}

// =============================================================================
// 11. PowerOfTwoBucketWidth
// =============================================================================

TEST(FixedLatencyHistogramTest, PowerOfTwoBucketWidth) {
  // Power-of-two bucket width — compiler should emit right-shift.
  mk::ds::FixedLatencyHistogram<64, 16> h;

  h.record(0);  // bucket 0 (0 >> 4 = 0)
  h.record(15); // bucket 0 (15 >> 4 = 0)
  h.record(16); // bucket 1 (16 >> 4 = 1)
  h.record(31); // bucket 1 (31 >> 4 = 1)
  h.record(32); // bucket 2 (32 >> 4 = 2)

  EXPECT_EQ(h.count_at(0), 2U);
  EXPECT_EQ(h.count_at(1), 2U);
  EXPECT_EQ(h.count_at(2), 1U);
  EXPECT_EQ(h.total_count(), 5U);
}

// =============================================================================
// 12. ZeroValue
// =============================================================================

TEST(FixedLatencyHistogramTest, ZeroValue) {
  Hist h;
  h.record(0);

  EXPECT_EQ(h.total_count(), 1U);
  EXPECT_EQ(h.min_value(), 0U);
  EXPECT_EQ(h.max_value(), 0U);
  EXPECT_EQ(h.count_at(0), 1U);
  EXPECT_EQ(h.percentile(50.0), 10U); // upper boundary of bucket 0
}

// =============================================================================
// 13. CompileTimeConstants
// =============================================================================

TEST(FixedLatencyHistogramTest, CompileTimeConstants) {
  // Verify constexpr static methods work at compile time.
  static_assert(Hist::num_buckets() == 16);
  static_assert(Hist::bucket_width() == 10);
  static_assert(Hist::max_range() == 160);

  using BigHist = mk::ds::FixedLatencyHistogram<1024, 16>;
  static_assert(BigHist::num_buckets() == 1024);
  static_assert(BigHist::bucket_width() == 16);
  static_assert(BigHist::max_range() == 16384);
}

// =============================================================================
// 14. Death tests — precondition violations (Debug builds only)
// =============================================================================

TEST(FixedLatencyHistogramDeathTest, CountAtOutOfRange) {
  EXPECT_DEATH(
      {
        const Hist h;
        (void)h.count_at(16); // NumBuckets = 16, max valid index = 15
      },
      "");
}

} // namespace
