/**
 * @file nano_clock_test.cpp
 * @brief GTest-based tests for nano_clock -- nanosecond timing utilities.
 *
 * Google Test concepts used:
 *
 *   EXPECT_GT(a, b):
 *     Greater-than comparison. Used to verify timestamps are positive
 *     and that time moves forward between calls.
 *
 *   EXPECT_LE(a, b):
 *     Less-or-equal. Used for upper-bound sanity checks.
 *
 * Test plan:
 *   1. monotonic_nanos returns positive value
 *   2. monotonic_nanos is monotonically increasing
 *   3. realtime_nanos returns a plausible Unix timestamp
 *   4. rdtsc returns a non-zero value
 *   5. Elapsed time measurement sanity check
 */

#include "sys/nano_clock.hpp"

#include <gtest/gtest.h>

#include <thread>

// ============================================================================
// 1. monotonic_nanos returns positive value
// ============================================================================

TEST(NanoClockTest, MonotonicReturnsPositive) {
  const std::int64_t now = mk::sys::monotonic_nanos();
  EXPECT_GT(now, 0);
}

// ============================================================================
// 2. monotonic_nanos is monotonically increasing
// ============================================================================
//
// Two consecutive calls must return non-decreasing values.
// In practice, they should be strictly increasing because even
// the call overhead takes a few nanoseconds.

TEST(NanoClockTest, MonotonicIsIncreasing) {
  const std::int64_t t1 = mk::sys::monotonic_nanos();
  const std::int64_t t2 = mk::sys::monotonic_nanos();
  EXPECT_GE(t2, t1);
}

// ============================================================================
// 3. realtime_nanos returns a plausible Unix timestamp
// ============================================================================
//
// Unix epoch nanoseconds for 2025-01-01 00:00:00 UTC = 1735689600 * 1e9.
// The value must be at least that large (we are past 2025).

TEST(NanoClockTest, RealtimeIsPlausible) {
  const std::int64_t now = mk::sys::realtime_nanos();

  // 2025-01-01 00:00:00 UTC in nanoseconds
  constexpr std::int64_t kJan2025 = 1'735'689'600LL * 1'000'000'000;
  EXPECT_GT(now, kJan2025);
}

// ============================================================================
// 4. rdtsc returns a non-zero value
// ============================================================================

TEST(NanoClockTest, RdtscReturnsNonZero) {
  const std::uint64_t tsc = mk::sys::rdtsc();
  EXPECT_GT(tsc, 0U);
}

// ============================================================================
// 5. Elapsed time measurement sanity check
// ============================================================================
//
// Sleep for ~1ms, measure elapsed time with monotonic_nanos().
// Verify the measured duration is at least 1ms (1'000'000 ns)
// and no more than 50ms (generous upper bound for OS scheduling jitter).

TEST(NanoClockTest, ElapsedTimeMeasurement) {
  auto start = mk::sys::monotonic_nanos();
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  auto end = mk::sys::monotonic_nanos();

  auto elapsed_ns = end - start;
  EXPECT_GE(elapsed_ns, 1'000'000)
      << "Elapsed should be at least 1ms (1'000'000 ns)";
  EXPECT_LE(elapsed_ns, 50'000'000) << "Elapsed should not exceed 50ms";
}
