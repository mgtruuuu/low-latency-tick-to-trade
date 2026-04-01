/**
 * @file wire_reader_test.cpp
 * @brief GTest-based tests for wire_reader.hpp.
 *
 * Test plan:
 *   1. read_u32_be known value
 *   2. read_u32_le known value
 *   3. read_u16_be / read_u64_be known values
 *   4. read_bytes to dst span
 *   5. bounds overflow — returns nullopt, pos unchanged
 *   6. consumed() / remaining() observers
 *   7. round-trip: WireWriter → WireReader
 */

#include "net/wire_reader.hpp"
#include "net/wire_writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

// ============================================================================
// 1. read_u32_be known value
// ============================================================================

TEST(WireReaderTest, ReadU32BeKnownValue) {
  // 0x01020304 in big-endian byte order.
  const std::array<std::byte, 4> data{std::byte{0x01}, std::byte{0x02},
                                      std::byte{0x03}, std::byte{0x04}};
  mk::net::WireReader r{.buf = std::span<const std::byte>{data}};

  auto val = r.read_u32_be();
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, 0x01020304U); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(r.consumed(), 4U);
}

// ============================================================================
// 2. read_u32_le known value
// ============================================================================

TEST(WireReaderTest, ReadU32LeKnownValue) {
  // 0x01020304 in little-endian byte order: LSB first.
  const std::array<std::byte, 4> data{std::byte{0x04}, std::byte{0x03},
                                      std::byte{0x02}, std::byte{0x01}};
  mk::net::WireReader r{.buf = std::span<const std::byte>{data}};

  auto val = r.read_u32_le();
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, 0x01020304U); // NOLINT(bugprone-unchecked-optional-access)
}

// ============================================================================
// 3. read_u16_be / read_u64_be known values
// ============================================================================

TEST(WireReaderTest, ReadU16BeKnownValue) {
  const std::array<std::byte, 2> data{std::byte{0xCA}, std::byte{0xFE}};
  mk::net::WireReader r{.buf = std::span<const std::byte>{data}};

  auto val = r.read_u16_be();
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, 0xCAFEU); // NOLINT(bugprone-unchecked-optional-access)
}

TEST(WireReaderTest, ReadU64BeKnownValue) {
  const std::array<std::byte, 8> data{
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
      std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};
  mk::net::WireReader r{.buf = std::span<const std::byte>{data}};

  auto val = r.read_u64_be();
  ASSERT_TRUE(val.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(*val, 0x0102030405060708ULL);
}

// ============================================================================
// 4. read_bytes to dst span
// ============================================================================

TEST(WireReaderTest, ReadBytesRawSpan) {
  const std::array<std::byte, 3> data{std::byte{0xAA}, std::byte{0xBB},
                                      std::byte{0xCC}};
  mk::net::WireReader r{.buf = std::span<const std::byte>{data}};

  std::array<std::byte, 3> dst{};
  ASSERT_TRUE(r.read_bytes(dst));

  EXPECT_EQ(dst[0], std::byte{0xAA});
  EXPECT_EQ(dst[1], std::byte{0xBB});
  EXPECT_EQ(dst[2], std::byte{0xCC});
  EXPECT_EQ(r.consumed(), 3U);
}

// ============================================================================
// 5. bounds overflow
// ============================================================================

TEST(WireReaderTest, OverflowReturnsNullopt) {
  // Only 3 bytes available, but trying to read 4.
  const std::array<std::byte, 3> data{std::byte{0x01}, std::byte{0x02},
                                      std::byte{0x03}};
  mk::net::WireReader r{.buf = std::span<const std::byte>{data}};

  auto val = r.read_u32_be();
  EXPECT_FALSE(val.has_value());
  EXPECT_EQ(r.consumed(), 0U);
}

TEST(WireReaderTest, SequentialOverflow) {
  const std::array<std::byte, 6> data{};
  mk::net::WireReader r{.buf = std::span<const std::byte>{data}};

  ASSERT_TRUE(r.read_u32_be().has_value()); // 4 bytes, OK
  EXPECT_EQ(r.remaining(), 2U);
  EXPECT_FALSE(r.read_u32_be().has_value()); // 4 more, no room
  EXPECT_EQ(r.consumed(), 4U);               // pos unchanged from failed read
}

// ============================================================================
// 6. observers
// ============================================================================

TEST(WireReaderTest, Observers) {
  const std::array<std::byte, 16> data{};
  mk::net::WireReader r{.buf = std::span<const std::byte>{data}};

  EXPECT_EQ(r.consumed(), 0U);
  EXPECT_EQ(r.remaining(), 16U);

  r.read_u32_be();
  EXPECT_EQ(r.consumed(), 4U);
  EXPECT_EQ(r.remaining(), 12U);
}

// ============================================================================
// 7. round-trip: WireWriter → WireReader
// ============================================================================

TEST(WireReaderTest, RoundTripBe) {
  std::array<std::byte, 32> buf{};

  // Write
  mk::net::WireWriter w{.buf = buf};
  ASSERT_TRUE(w.write_u16_be(0xCAFEU));
  ASSERT_TRUE(w.write_u32_be(0xDEADBEEFU));
  ASSERT_TRUE(w.write_u64_be(0x0123456789ABCDEFULL));

  // Read back
  mk::net::WireReader r{.buf =
                            std::span<const std::byte>{buf}.first(w.written())};
  auto v16 = r.read_u16_be();
  auto v32 = r.read_u32_be();
  auto v64 = r.read_u64_be();

  ASSERT_TRUE(v16.has_value());
  ASSERT_TRUE(v32.has_value());
  ASSERT_TRUE(v64.has_value());
  EXPECT_EQ(*v16, 0xCAFEU);     // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(*v32, 0xDEADBEEFU); // NOLINT(bugprone-unchecked-optional-access)
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(*v64, 0x0123456789ABCDEFULL);
  EXPECT_EQ(r.remaining(), 0U);
}

TEST(WireReaderTest, RoundTripLe) {
  std::array<std::byte, 32> buf{};

  mk::net::WireWriter w{.buf = buf};
  ASSERT_TRUE(w.write_u16_le(0xBEEFU));
  ASSERT_TRUE(w.write_u32_le(0x12345678U));
  ASSERT_TRUE(w.write_u64_le(0xFEDCBA9876543210ULL));

  mk::net::WireReader r{.buf =
                            std::span<const std::byte>{buf}.first(w.written())};
  auto v16 = r.read_u16_le();
  auto v32 = r.read_u32_le();
  auto v64 = r.read_u64_le();

  ASSERT_TRUE(v16.has_value());
  ASSERT_TRUE(v32.has_value());
  ASSERT_TRUE(v64.has_value());
  EXPECT_EQ(*v16, 0xBEEFU);     // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(*v32, 0x12345678U); // NOLINT(bugprone-unchecked-optional-access)
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(*v64, 0xFEDCBA9876543210ULL);
  EXPECT_EQ(r.remaining(), 0U);
}
