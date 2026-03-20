/**
 * @file wire_writer_test.cpp
 * @brief GTest-based tests for wire_writer.hpp.
 *
 * Test plan:
 *   1. write_u32_be known value — verify raw bytes match BE layout
 *   2. write_u32_le known value — verify raw bytes match LE layout
 *   3. write_u16_be / write_u64_be known values
 *   4. write_bytes raw span
 *   5. bounds overflow — returns false, pos unchanged
 *   6. write_bytes empty span — precondition violation (death test)
 *   7. written() / remaining() observers
 */

#include "net/wire_writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

// ============================================================================
// 1. write_u32_be known value
// ============================================================================

TEST(WireWriterTest, WriteU32BeKnownValue) {
  std::array<std::byte, 16> buf{};
  mk::net::WireWriter w{.buf = buf};

  ASSERT_TRUE(w.write_u32_be(0x01020304U));

  // Big-endian: most significant byte first.
  EXPECT_EQ(buf[0], std::byte{0x01});
  EXPECT_EQ(buf[1], std::byte{0x02});
  EXPECT_EQ(buf[2], std::byte{0x03});
  EXPECT_EQ(buf[3], std::byte{0x04});
  EXPECT_EQ(w.written(), 4U);
}

// ============================================================================
// 2. write_u32_le known value
// ============================================================================

TEST(WireWriterTest, WriteU32LeKnownValue) {
  std::array<std::byte, 16> buf{};
  mk::net::WireWriter w{.buf = buf};

  ASSERT_TRUE(w.write_u32_le(0x01020304U));

  // Little-endian: least significant byte first.
  EXPECT_EQ(buf[0], std::byte{0x04});
  EXPECT_EQ(buf[1], std::byte{0x03});
  EXPECT_EQ(buf[2], std::byte{0x02});
  EXPECT_EQ(buf[3], std::byte{0x01});
}

// ============================================================================
// 3. write_u16_be / write_u64_be known values
// ============================================================================

TEST(WireWriterTest, WriteU16BeKnownValue) {
  std::array<std::byte, 16> buf{};
  mk::net::WireWriter w{.buf = buf};

  ASSERT_TRUE(w.write_u16_be(0xCAFEU));

  EXPECT_EQ(buf[0], std::byte{0xCA});
  EXPECT_EQ(buf[1], std::byte{0xFE});
  EXPECT_EQ(w.written(), 2U);
}

TEST(WireWriterTest, WriteU64BeKnownValue) {
  std::array<std::byte, 16> buf{};
  mk::net::WireWriter w{.buf = buf};

  ASSERT_TRUE(w.write_u64_be(0x0102030405060708ULL));

  EXPECT_EQ(buf[0], std::byte{0x01});
  EXPECT_EQ(buf[1], std::byte{0x02});
  EXPECT_EQ(buf[2], std::byte{0x03});
  EXPECT_EQ(buf[3], std::byte{0x04});
  EXPECT_EQ(buf[4], std::byte{0x05});
  EXPECT_EQ(buf[5], std::byte{0x06});
  EXPECT_EQ(buf[6], std::byte{0x07});
  EXPECT_EQ(buf[7], std::byte{0x08});
  EXPECT_EQ(w.written(), 8U);
}

// ============================================================================
// 4. write_bytes raw span
// ============================================================================

TEST(WireWriterTest, WriteBytesRawSpan) {
  std::array<std::byte, 16> buf{};
  mk::net::WireWriter w{.buf = buf};

  const std::array<std::byte, 3> src{std::byte{0xAA}, std::byte{0xBB},
                                     std::byte{0xCC}};
  ASSERT_TRUE(w.write_bytes(src));

  EXPECT_EQ(buf[0], std::byte{0xAA});
  EXPECT_EQ(buf[1], std::byte{0xBB});
  EXPECT_EQ(buf[2], std::byte{0xCC});
  EXPECT_EQ(w.written(), 3U);
}

// ============================================================================
// 5. bounds overflow
// ============================================================================

TEST(WireWriterTest, OverflowReturnsFalse) {
  std::array<std::byte, 3> buf{};
  mk::net::WireWriter w{.buf = buf};

  // 4 bytes into 3-byte buffer should fail.
  EXPECT_FALSE(w.write_u32_be(0x12345678U));
  // pos must remain 0 (write was not applied).
  EXPECT_EQ(w.written(), 0U);
}

TEST(WireWriterTest, SequentialOverflow) {
  std::array<std::byte, 6> buf{};
  mk::net::WireWriter w{.buf = buf};

  ASSERT_TRUE(w.write_u32_be(0x11223344U)); // 4 bytes, OK
  EXPECT_EQ(w.remaining(), 2U);
  EXPECT_FALSE(w.write_u32_be(0x55667788U)); // 4 more bytes, no room
  EXPECT_EQ(w.written(), 4U);                // pos unchanged from failed write
}

// ============================================================================
// 6. write_bytes empty span — precondition violation (debug only)
// ============================================================================

#ifndef NDEBUG

TEST(WireWriterDeathTest, EmptySpanAborts) {
  // Using alias avoids template comma inside EXPECT_DEATH macro args.
  using Buf = std::array<std::byte, 4>;
  EXPECT_DEATH(
      {
        Buf buf{};
        mk::net::WireWriter w{.buf = buf};
        const std::span<const std::byte> empty{};
        (void)w.write_bytes(empty);
      },
      "");
}

#endif // NDEBUG

// ============================================================================
// 7. observers
// ============================================================================

TEST(WireWriterTest, Observers) {
  std::array<std::byte, 16> buf{};
  mk::net::WireWriter w{.buf = buf};

  EXPECT_EQ(w.written(), 0U);
  EXPECT_EQ(w.remaining(), 16U);

  w.write_u32_be(0);
  EXPECT_EQ(w.written(), 4U);
  EXPECT_EQ(w.remaining(), 12U);

  w.write_u64_be(0);
  EXPECT_EQ(w.written(), 12U);
  EXPECT_EQ(w.remaining(), 4U);
}
