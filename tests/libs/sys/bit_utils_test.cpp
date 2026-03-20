/**
 * @file bit_utils_test.cpp
 * @brief GTest-based tests for bit_utils.hpp — alignment and power-of-two ops.
 *
 * Test plan:
 *   1. align_up — basic cases, already aligned, edge cases (0, 1)
 *   2. align_down — basic cases, already aligned, edge cases
 *   3. is_aligned — aligned and misaligned addresses
 *   4. align_ptr_up — pointer alignment
 *   5. is_power_of_two — known values, 0, non-powers
 *   6. round_up_pow2 — basic cases, exact powers, edge cases (0, 1)
 *   7. constexpr evaluation (compile-time verification)
 */

#include "sys/bit_utils.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using mk::sys::align_down;
using mk::sys::align_ptr_up;
using mk::sys::align_up;
using mk::sys::is_aligned;
using mk::sys::is_power_of_two;
using mk::sys::round_up_pow2;

// ============================================================================
// 1. align_up
// ============================================================================

TEST(BitUtilsTest, AlignUpBasic) {
  // 7 rounded up to alignment 8 → 8
  EXPECT_EQ(align_up(7, 8), 8U);
  // 9 rounded up to alignment 8 → 16
  EXPECT_EQ(align_up(9, 8), 16U);
  // 13 rounded up to alignment 4 → 16
  EXPECT_EQ(align_up(13, 4), 16U);
  // 1 rounded up to alignment 64 → 64
  EXPECT_EQ(align_up(1, 64), 64U);
}

TEST(BitUtilsTest, AlignUpAlreadyAligned) {
  // Already aligned values should be unchanged.
  EXPECT_EQ(align_up(8, 8), 8U);
  EXPECT_EQ(align_up(64, 16), 64U);
  EXPECT_EQ(align_up(4096, 4096), 4096U);
}

TEST(BitUtilsTest, AlignUpEdgeCases) {
  // 0 aligned to anything is 0.
  EXPECT_EQ(align_up(0, 8), 0U);
  EXPECT_EQ(align_up(0, 4096), 0U);
  // Alignment of 1: everything is already aligned.
  EXPECT_EQ(align_up(7, 1), 7U);
  EXPECT_EQ(align_up(123, 1), 123U);
}

// ============================================================================
// 2. align_down
// ============================================================================

TEST(BitUtilsTest, AlignDownBasic) {
  // 7 rounded down to alignment 8 → 0
  EXPECT_EQ(align_down(7, 8), 0U);
  // 9 rounded down to alignment 8 → 8
  EXPECT_EQ(align_down(9, 8), 8U);
  // 15 rounded down to alignment 4 → 12
  EXPECT_EQ(align_down(15, 4), 12U);
  // 100 rounded down to alignment 64 → 64
  EXPECT_EQ(align_down(100, 64), 64U);
}

TEST(BitUtilsTest, AlignDownAlreadyAligned) {
  EXPECT_EQ(align_down(8, 8), 8U);
  EXPECT_EQ(align_down(64, 16), 64U);
  EXPECT_EQ(align_down(4096, 4096), 4096U);
}

TEST(BitUtilsTest, AlignDownEdgeCases) {
  EXPECT_EQ(align_down(0, 8), 0U);
  EXPECT_EQ(align_down(0, 4096), 0U);
  // Alignment of 1: unchanged.
  EXPECT_EQ(align_down(7, 1), 7U);
  EXPECT_EQ(align_down(123, 1), 123U);
}

// ============================================================================
// 3. is_aligned
// ============================================================================

TEST(BitUtilsTest, IsAlignedTrue) {
  EXPECT_TRUE(is_aligned(0, 8));
  EXPECT_TRUE(is_aligned(8, 8));
  EXPECT_TRUE(is_aligned(16, 8));
  EXPECT_TRUE(is_aligned(64, 64));
  EXPECT_TRUE(is_aligned(4096, 4096));
}

TEST(BitUtilsTest, IsAlignedFalse) {
  EXPECT_FALSE(is_aligned(1, 8));
  EXPECT_FALSE(is_aligned(7, 8));
  EXPECT_FALSE(is_aligned(9, 8));
  EXPECT_FALSE(is_aligned(63, 64));
  EXPECT_FALSE(is_aligned(4095, 4096));
}

TEST(BitUtilsTest, IsAlignedOneAlwaysTrue) {
  // Every address is aligned to 1.
  EXPECT_TRUE(is_aligned(0, 1));
  EXPECT_TRUE(is_aligned(1, 1));
  EXPECT_TRUE(is_aligned(42, 1));
}

// ============================================================================
// 4. align_ptr_up
// ============================================================================

TEST(BitUtilsTest, AlignPtrUpBasic) {
  // Use a known misaligned address (cast from integer).
  auto *base = reinterpret_cast<char *>(std::uintptr_t{0x1001});
  auto *aligned = align_ptr_up(base, 16);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(aligned), 0x1010U);
}

TEST(BitUtilsTest, AlignPtrUpAlreadyAligned) {
  auto *base = reinterpret_cast<char *>(std::uintptr_t{0x1000});
  auto *aligned = align_ptr_up(base, 16);
  // Already aligned — should be unchanged.
  EXPECT_EQ(aligned, base);
}

TEST(BitUtilsTest, AlignPtrUpPreservesType) {
  // Verify alignment works with int* (not just char*).
  auto *base = reinterpret_cast<int *>(std::uintptr_t{0x2004});
  auto *aligned = align_ptr_up(base, 64);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(aligned), 0x2040U);
}

// ============================================================================
// 5. is_power_of_two
// ============================================================================

TEST(BitUtilsTest, IsPowerOfTwoTrue) {
  EXPECT_TRUE(is_power_of_two(1));
  EXPECT_TRUE(is_power_of_two(2));
  EXPECT_TRUE(is_power_of_two(4));
  EXPECT_TRUE(is_power_of_two(8));
  EXPECT_TRUE(is_power_of_two(16));
  EXPECT_TRUE(is_power_of_two(1024));
  EXPECT_TRUE(is_power_of_two(1U << 20)); // 1MB
  EXPECT_TRUE(is_power_of_two(1U << 31)); // 2GB
}

TEST(BitUtilsTest, IsPowerOfTwoFalse) {
  EXPECT_FALSE(is_power_of_two(0));
  EXPECT_FALSE(is_power_of_two(3));
  EXPECT_FALSE(is_power_of_two(5));
  EXPECT_FALSE(is_power_of_two(6));
  EXPECT_FALSE(is_power_of_two(7));
  EXPECT_FALSE(is_power_of_two(12));
  EXPECT_FALSE(is_power_of_two(100));
  EXPECT_FALSE(is_power_of_two(0xFFFFFFFFU));
}

// ============================================================================
// 6. round_up_pow2
// ============================================================================

TEST(BitUtilsTest, RoundUpPow2Basic) {
  EXPECT_EQ(round_up_pow2(3), 4U);
  EXPECT_EQ(round_up_pow2(5), 8U);
  EXPECT_EQ(round_up_pow2(9), 16U);
  EXPECT_EQ(round_up_pow2(17), 32U);
  EXPECT_EQ(round_up_pow2(100), 128U);
  EXPECT_EQ(round_up_pow2(1000), 1024U);
}

TEST(BitUtilsTest, RoundUpPow2ExactPowers) {
  // Exact powers-of-two should be unchanged.
  EXPECT_EQ(round_up_pow2(1), 1U);
  EXPECT_EQ(round_up_pow2(2), 2U);
  EXPECT_EQ(round_up_pow2(4), 4U);
  EXPECT_EQ(round_up_pow2(8), 8U);
  EXPECT_EQ(round_up_pow2(16), 16U);
  EXPECT_EQ(round_up_pow2(1024), 1024U);
  EXPECT_EQ(round_up_pow2(1U << 20), 1U << 20);
}

TEST(BitUtilsTest, RoundUpPow2EdgeCases) {
  // 0 → 1 (matches std::bit_ceil: 2^0 = 1).
  EXPECT_EQ(round_up_pow2(0), 1U);
  // 1 → 1
  EXPECT_EQ(round_up_pow2(1), 1U);
  // Just above a power-of-two.
  EXPECT_EQ(round_up_pow2(1025), 2048U);
  // Just below a power-of-two.
  EXPECT_EQ(round_up_pow2(1023), 1024U);
}

// ============================================================================
// 7. constexpr evaluation
// ============================================================================
//
// Verify all functions are usable at compile time.
// If any function loses its constexpr qualifier, these static_asserts fail.

TEST(BitUtilsTest, ConstexprAlignUp) {
  static_assert(align_up(7, 8) == 8);
  static_assert(align_up(8, 8) == 8);
  static_assert(align_up(0, 64) == 0);
}

TEST(BitUtilsTest, ConstexprAlignDown) {
  static_assert(align_down(9, 8) == 8);
  static_assert(align_down(8, 8) == 8);
  static_assert(align_down(0, 64) == 0);
}

TEST(BitUtilsTest, ConstexprIsAligned) {
  static_assert(is_aligned(64, 64));
  static_assert(!is_aligned(63, 64));
  static_assert(is_aligned(0, 8));
}

TEST(BitUtilsTest, ConstexprIsPowerOfTwo) {
  static_assert(is_power_of_two(1));
  static_assert(is_power_of_two(1024));
  static_assert(!is_power_of_two(0));
  static_assert(!is_power_of_two(3));
}

TEST(BitUtilsTest, ConstexprRoundUpPow2) {
  static_assert(round_up_pow2(0) == 1);
  static_assert(round_up_pow2(1) == 1);
  static_assert(round_up_pow2(3) == 4);
  static_assert(round_up_pow2(16) == 16);
  static_assert(round_up_pow2(17) == 32);
}
