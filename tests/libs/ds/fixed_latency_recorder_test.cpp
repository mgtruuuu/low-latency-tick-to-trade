#include "ds/fixed_latency_recorder.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace {

// Small histogram config for fast tests.
using Recorder3 = mk::ds::FixedLatencyRecorder<3, 64, 10>;

// Stage indices (enum pattern matching intended usage).
enum Stage : std::size_t { kA = 0, kB = 1, kC = 2 };

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST(FixedLatencyRecorderTest, EmptyOnConstruction) {
  const Recorder3 rec;
  for (std::size_t i = 0; i < Recorder3::num_stages(); ++i) {
    EXPECT_TRUE(rec.histogram(i).empty());
    EXPECT_EQ(rec.histogram(i).total_count(), 0);
  }
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

TEST(FixedLatencyRecorderTest, RecordSingleStage) {
  Recorder3 rec;
  rec.record(kA, 50);
  rec.record(kA, 60);

  EXPECT_EQ(rec.histogram(kA).total_count(), 2);
  EXPECT_TRUE(rec.histogram(kB).empty());
  EXPECT_TRUE(rec.histogram(kC).empty());
}

TEST(FixedLatencyRecorderTest, RecordMultipleStages) {
  Recorder3 rec;
  rec.record(kA, 10);
  rec.record(kB, 20);
  rec.record(kB, 30);
  rec.record(kC, 40);
  rec.record(kC, 50);
  rec.record(kC, 60);

  EXPECT_EQ(rec.histogram(kA).total_count(), 1);
  EXPECT_EQ(rec.histogram(kB).total_count(), 2);
  EXPECT_EQ(rec.histogram(kC).total_count(), 3);
}

// ---------------------------------------------------------------------------
// Histogram accessor
// ---------------------------------------------------------------------------

TEST(FixedLatencyRecorderTest, HistogramAccessor) {
  Recorder3 rec;
  rec.record(kA, 15);
  rec.record(kA, 25);
  rec.record(kA, 35);

  const auto &hist = rec.histogram(kA);
  EXPECT_EQ(hist.total_count(), 3);
  EXPECT_EQ(hist.min_value(), 15);
  EXPECT_EQ(hist.max_value(), 35);
  // With bucket width 10: bucket 1 (10-19) has 15, bucket 2 (20-29) has 25,
  // bucket 3 (30-39) has 35. target = 50/100 * 3 = 1 (truncated).
  // Bucket 1 cumulative = 1 >= 1 → returns upper boundary (1+1)*10 = 20.
  EXPECT_EQ(hist.percentile(50.0), 20);
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

TEST(FixedLatencyRecorderTest, ClearAll) {
  Recorder3 rec;
  rec.record(kA, 10);
  rec.record(kB, 20);
  rec.record(kC, 30);

  rec.clear();

  for (std::size_t i = 0; i < Recorder3::num_stages(); ++i) {
    EXPECT_TRUE(rec.histogram(i).empty());
  }
}

TEST(FixedLatencyRecorderTest, ClearSingleStage) {
  Recorder3 rec;
  rec.record(kA, 10);
  rec.record(kB, 20);
  rec.record(kC, 30);

  rec.clear(kB);

  EXPECT_EQ(rec.histogram(kA).total_count(), 1);
  EXPECT_TRUE(rec.histogram(kB).empty());
  EXPECT_EQ(rec.histogram(kC).total_count(), 1);
}

// ---------------------------------------------------------------------------
// Compile-time constants
// ---------------------------------------------------------------------------

TEST(FixedLatencyRecorderTest, CompileTimeConstants) {
  EXPECT_EQ(Recorder3::num_stages(), 3);
  EXPECT_EQ(Recorder3::num_buckets(), 64);
  EXPECT_EQ(Recorder3::bucket_width(), 10);
}

TEST(FixedLatencyRecorderTest, DefaultTemplateParameters) {
  using DefaultRec = mk::ds::FixedLatencyRecorder<2>;
  EXPECT_EQ(DefaultRec::num_stages(), 2);
  EXPECT_EQ(DefaultRec::num_buckets(), 1024);
  EXPECT_EQ(DefaultRec::bucket_width(), 16);
}

// ---------------------------------------------------------------------------
// Large volume
// ---------------------------------------------------------------------------

TEST(FixedLatencyRecorderTest, LargeVolume) {
  Recorder3 rec;
  constexpr std::uint64_t kSamples = 100'000;
  for (std::uint64_t i = 0; i < kSamples; ++i) {
    rec.record(kA, i % 640); // 64 buckets × 10 width = 640 range
  }
  EXPECT_EQ(rec.histogram(kA).total_count(), kSamples);
  EXPECT_EQ(rec.histogram(kA).min_value(), 0);
  EXPECT_EQ(rec.histogram(kA).max_value(), 639);
}

// ---------------------------------------------------------------------------
// Histogram type alias
// ---------------------------------------------------------------------------

TEST(FixedLatencyRecorderTest, HistogramTypeAlias) {
  // Verify the Histogram type alias is correct.
  static_assert(
      std::is_same_v<Recorder3::Histogram,
                     mk::ds::FixedLatencyHistogram<64, 10>>);
}

// ---------------------------------------------------------------------------
// Death tests
// ---------------------------------------------------------------------------

using FixedLatencyRecorderDeathTest = ::testing::Test;

TEST_F(FixedLatencyRecorderDeathTest, RecordOutOfRange) {
  Recorder3 rec;
  EXPECT_DEATH(rec.record(3, 100), "");
}

TEST_F(FixedLatencyRecorderDeathTest, HistogramOutOfRange) {
  const Recorder3 rec;
  EXPECT_DEATH((void)rec.histogram(3), "");
}

TEST_F(FixedLatencyRecorderDeathTest, ClearStageOutOfRange) {
  Recorder3 rec;
  EXPECT_DEATH(rec.clear(3), "");
}

} // namespace
