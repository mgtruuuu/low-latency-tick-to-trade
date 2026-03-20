/**
 * @file xorshift64_test.cpp
 * @brief GTest-based tests for xorshift64.hpp — fast PRNG utility.
 *
 * Test plan:
 *   1. Determinism — same seed produces same sequence
 *   2. State progression — consecutive calls change state
 *   3. Non-zero output — first N outputs are all non-zero
 *   4. Constexpr — compile-time evaluation
 *   5. Zero seed — aborts (death test)
 */

#include "sys/xorshift64.hpp"

#include <gtest/gtest.h>

using mk::sys::Xorshift64;

// ============================================================================
// 1. Determinism
// ============================================================================

TEST(Xorshift64Test, SameSeedSameSequence) {
  Xorshift64 a{42};
  Xorshift64 b{42};

  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(a(), b());
  }
}

TEST(Xorshift64Test, DifferentSeedDifferentSequence) {
  Xorshift64 a{1};
  Xorshift64 b{2};

  // First output should differ (extremely unlikely to collide).
  EXPECT_NE(a(), b());
}

// ============================================================================
// 2. State progression
// ============================================================================

TEST(Xorshift64Test, StateChangesAfterEachCall) {
  Xorshift64 rng{123};

  const auto s0 = rng.state();
  static_cast<void>(rng());
  const auto s1 = rng.state();
  static_cast<void>(rng());
  const auto s2 = rng.state();

  EXPECT_NE(s0, s1);
  EXPECT_NE(s1, s2);
  EXPECT_NE(s0, s2);
}

// ============================================================================
// 3. Non-zero output
// ============================================================================

TEST(Xorshift64Test, FirstThousandOutputsNonZero) {
  Xorshift64 rng{0xDEAD'BEEF'CAFE'BABEULL};

  for (int i = 0; i < 1000; ++i) {
    // xorshift64 never produces 0 (state 0 is absorbing and excluded by
    // the non-zero seed precondition).
    EXPECT_NE(rng(), 0U);
  }
}

// ============================================================================
// 4. Constexpr
// ============================================================================

TEST(Xorshift64Test, ConstexprEvaluation) {
  // Verify the PRNG works at compile time.
  constexpr auto kVal = []() {
    Xorshift64 rng{7};
    return rng();
  }();

  static_assert(kVal != 0, "constexpr xorshift64 should produce non-zero");
  EXPECT_NE(kVal, 0U);
}

// ============================================================================
// 5. Zero seed (death test)
// ============================================================================

using Xorshift64DeathTest = ::testing::Test;

TEST(Xorshift64DeathTest, ZeroSeedAborts) {
  EXPECT_DEATH(Xorshift64{0}, "");
}
