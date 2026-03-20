/**
 * @file endian_test.cpp
 * @brief GTest-based tests for endian.hpp — byte-swap and endian conversion.
 *
 * Test plan:
 *   1. bswap known-value tests (16/32/64-bit)
 *   2. bswap round-trip: swap(swap(x)) == x
 *   3. bswap edge cases: 0, max value
 *   4. host_to_be / be_to_host symmetry
 *   5. host_to_le / le_to_host symmetry
 *   6. constexpr evaluation (compile-time swap)
 */

#include "sys/endian.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

// ============================================================================
// 1. bswap known-value tests
// ============================================================================

TEST(EndianTest, Bswap16KnownValue) {
  // 0x0102 → 0x0201
  EXPECT_EQ(mk::sys::bswap16(0x0102U), 0x0201U);
  // 0xAABB → 0xBBAA
  EXPECT_EQ(mk::sys::bswap16(0xAABBU), 0xBBAAU);
}

TEST(EndianTest, Bswap32KnownValue) {
  // 0x01020304 → 0x04030201
  EXPECT_EQ(mk::sys::bswap32(0x01020304U), 0x04030201U);
  // 0xDEADBEEF → 0xEFBEADDE
  EXPECT_EQ(mk::sys::bswap32(0xDEADBEEFU), 0xEFBEADDEU);
}

TEST(EndianTest, Bswap64KnownValue) {
  // 0x0102030405060708 → 0x0807060504030201
  EXPECT_EQ(mk::sys::bswap64(0x0102030405060708ULL), 0x0807060504030201ULL);
}

// ============================================================================
// 2. bswap round-trip: swap(swap(x)) == x
// ============================================================================

TEST(EndianTest, Bswap16RoundTrip) {
  constexpr std::uint16_t kVal = 0x1234U;
  EXPECT_EQ(mk::sys::bswap16(mk::sys::bswap16(kVal)), kVal);
}

TEST(EndianTest, Bswap32RoundTrip) {
  constexpr std::uint32_t kVal = 0x12345678U;
  EXPECT_EQ(mk::sys::bswap32(mk::sys::bswap32(kVal)), kVal);
}

TEST(EndianTest, Bswap64RoundTrip) {
  constexpr std::uint64_t kVal = 0x123456789ABCDEF0ULL;
  EXPECT_EQ(mk::sys::bswap64(mk::sys::bswap64(kVal)), kVal);
}

// ============================================================================
// 3. bswap edge cases: 0, max value
// ============================================================================

TEST(EndianTest, BswapZero) {
  EXPECT_EQ(mk::sys::bswap16(0), 0U);
  EXPECT_EQ(mk::sys::bswap32(0), 0U);
  EXPECT_EQ(mk::sys::bswap64(0), 0ULL);
}

TEST(EndianTest, BswapMaxValue) {
  // All-ones stays all-ones after swap.
  EXPECT_EQ(mk::sys::bswap16(std::numeric_limits<std::uint16_t>::max()),
            std::numeric_limits<std::uint16_t>::max());
  EXPECT_EQ(mk::sys::bswap32(std::numeric_limits<std::uint32_t>::max()),
            std::numeric_limits<std::uint32_t>::max());
  EXPECT_EQ(mk::sys::bswap64(std::numeric_limits<std::uint64_t>::max()),
            std::numeric_limits<std::uint64_t>::max());
}

// ============================================================================
// 4. host_to_be / be_to_host symmetry
// ============================================================================
//
// Regardless of host endianness, converting to BE and back must yield the
// original value. This tests the conversion pair, not a specific byte layout.

TEST(EndianTest, HostToBeRoundTrip16) {
  constexpr std::uint16_t kVal = 0xCAFEU;
  EXPECT_EQ(mk::sys::be16_to_host(mk::sys::host_to_be16(kVal)), kVal);
}

TEST(EndianTest, HostToBeRoundTrip32) {
  constexpr std::uint32_t kVal = 0xCAFEBABEU;
  EXPECT_EQ(mk::sys::be32_to_host(mk::sys::host_to_be32(kVal)), kVal);
}

TEST(EndianTest, HostToBeRoundTrip64) {
  constexpr std::uint64_t kVal = 0xCAFEBABEDEADBEEFULL;
  EXPECT_EQ(mk::sys::be64_to_host(mk::sys::host_to_be64(kVal)), kVal);
}

// ============================================================================
// 5. host_to_le / le_to_host symmetry
// ============================================================================

TEST(EndianTest, HostToLeRoundTrip16) {
  constexpr std::uint16_t kVal = 0xBEEFU;
  EXPECT_EQ(mk::sys::le16_to_host(mk::sys::host_to_le16(kVal)), kVal);
}

TEST(EndianTest, HostToLeRoundTrip32) {
  constexpr std::uint32_t kVal = 0xDEADBEEFU;
  EXPECT_EQ(mk::sys::le32_to_host(mk::sys::host_to_le32(kVal)), kVal);
}

TEST(EndianTest, HostToLeRoundTrip64) {
  constexpr std::uint64_t kVal = 0x0123456789ABCDEFULL;
  EXPECT_EQ(mk::sys::le64_to_host(mk::sys::host_to_le64(kVal)), kVal);
}

// ============================================================================
// 6. constexpr evaluation
// ============================================================================
//
// Verify that all functions are usable at compile time.
// If any function is not constexpr, these static_asserts fail to compile.

TEST(EndianTest, ConstexprBswap) {
  static_assert(mk::sys::bswap16(0x0102U) == 0x0201U);
  static_assert(mk::sys::bswap32(0x01020304U) == 0x04030201U);
  static_assert(mk::sys::bswap64(0x0102030405060708ULL) ==
                0x0807060504030201ULL);
}

TEST(EndianTest, ConstexprRoundTrip) {
  static_assert(mk::sys::be32_to_host(mk::sys::host_to_be32(0x12345678U)) ==
                0x12345678U);
  static_assert(mk::sys::le32_to_host(mk::sys::host_to_le32(0x12345678U)) ==
                0x12345678U);
}
