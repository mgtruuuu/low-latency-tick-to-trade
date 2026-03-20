/**
 * @file message_codec_test.cpp
 * @brief GTest-based tests for message_codec.hpp.
 *
 * Test plan:
 *   1.  Round-trip: pack → unpack, all fields match
 *   2.  Empty payload round-trip
 *   3.  Max payload (64 KB boundary) succeeds
 *   4.  Payload exceeds max_payload_size → pack returns 0
 *   5.  Output buffer too small → pack returns 0
 *   6.  Wrong magic → unpack returns false
 *   7.  Truncated header → unpack returns false
 *   8.  payload_len exceeds remaining bytes → unpack returns false
 *   9.  payload_len exceeds max_payload_size → unpack returns false
 *  10.  Zero-copy: payload span points into original buffer
 *  11.  Trailing bytes tolerated by unpack
 */

#include "net/message_codec.hpp"
#include "net/wire_writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// ============================================================================
// Helpers
// ============================================================================

namespace {

/// Convenience: create a payload span from a string literal (excluding '\0').
std::span<const std::byte> bytes_from(const char *s, std::size_t n) {
  return std::as_bytes(std::span{s, n});
}

} // namespace

// ============================================================================
// 1. Round-trip: pack → unpack, all fields match
// ============================================================================

TEST(MessageCodecTest, RoundTripAllFields) {
  std::array<std::byte, 256> buf{};
  const char text[] = "hello";
  const auto payload = bytes_from(text, 5);

  const auto written =
      mk::net::pack_message(buf, /*version=*/3, /*msg_type=*/42,
                            /*flags=*/0xFF00FF00U, payload);
  ASSERT_NE(written, 0U);
  EXPECT_EQ(written, mk::net::kMessageHeaderSize + 5);

  mk::net::ParsedMessageView parsed;
  ASSERT_TRUE(mk::net::unpack_message(
      std::span<const std::byte>{buf}.first(written), parsed));

  EXPECT_EQ(parsed.header.magic, mk::net::kMessageMagic);
  EXPECT_EQ(parsed.header.version, 3);
  EXPECT_EQ(parsed.header.msg_type, 42);
  EXPECT_EQ(parsed.header.payload_len, 5U);
  EXPECT_EQ(parsed.header.flags, 0xFF00FF00U);
  ASSERT_EQ(parsed.payload.size(), 5U);
  EXPECT_EQ(std::memcmp(parsed.payload.data(), text, 5), 0);
}

// ============================================================================
// 2. Empty payload round-trip
// ============================================================================

TEST(MessageCodecTest, EmptyPayload) {
  std::array<std::byte, 64> buf{};
  std::span<const std::byte> const empty{};

  const auto written = mk::net::pack_message(buf, /*version=*/1, /*msg_type=*/0,
                                             /*flags=*/0, empty);
  ASSERT_NE(written, 0U);
  EXPECT_EQ(written, mk::net::kMessageHeaderSize);

  mk::net::ParsedMessageView parsed;
  ASSERT_TRUE(mk::net::unpack_message(
      std::span<const std::byte>{buf}.first(written), parsed));

  EXPECT_EQ(parsed.header.payload_len, 0U);
  EXPECT_TRUE(parsed.payload.empty());
}

// ============================================================================
// 3. Max payload (64 KB) succeeds
// ============================================================================

TEST(MessageCodecTest, MaxPayloadSucceeds) {
  const std::size_t total =
      mk::net::kMessageHeaderSize + mk::net::kMaxPayloadSize;
  std::vector<std::byte> buf(total);
  std::vector<std::byte> payload(mk::net::kMaxPayloadSize, std::byte{0xAB});

  const auto written = mk::net::pack_message(buf, /*version=*/1, /*msg_type=*/1,
                                             /*flags=*/0, payload);
  ASSERT_NE(written, 0U);
  EXPECT_EQ(written, total);

  mk::net::ParsedMessageView parsed;
  ASSERT_TRUE(mk::net::unpack_message(
      std::span<const std::byte>{buf}.first(written), parsed));
  EXPECT_EQ(parsed.header.payload_len,
            static_cast<std::uint32_t>(mk::net::kMaxPayloadSize));
  ASSERT_EQ(parsed.payload.size(), mk::net::kMaxPayloadSize);
}

// ============================================================================
// 4. Payload exceeds max_payload_size → pack returns 0
// ============================================================================

TEST(MessageCodecTest, PackRejectsOversizedPayload) {
  const std::size_t over = mk::net::kMaxPayloadSize + 1;
  std::vector<std::byte> buf(mk::net::kMessageHeaderSize + over);
  std::vector<std::byte> payload(over, std::byte{0x00});

  const auto written = mk::net::pack_message(buf, /*version=*/1, /*msg_type=*/1,
                                             /*flags=*/0, payload);
  EXPECT_EQ(written, 0U);
}

// ============================================================================
// 5. Output buffer too small → pack returns 0
// ============================================================================

TEST(MessageCodecTest, PackRejectsSmallBuffer) {
  // Header alone is 16 bytes; buffer of 10 is too small even for header.
  std::array<std::byte, 10> buf{};
  std::span<const std::byte> const empty{};

  const auto written = mk::net::pack_message(buf, /*version=*/1, /*msg_type=*/1,
                                             /*flags=*/0, empty);
  EXPECT_EQ(written, 0U);
}

TEST(MessageCodecTest, PackRejectsBufferTooSmallForPayload) {
  // Buffer fits header (16) but not header + payload (16 + 4 = 20).
  std::array<std::byte, 18> buf{};
  const std::array<std::byte, 4> payload{};

  const auto written = mk::net::pack_message(buf, /*version=*/1, /*msg_type=*/1,
                                             /*flags=*/0, payload);
  EXPECT_EQ(written, 0U);
}

// ============================================================================
// 6. Wrong magic → unpack returns false
// ============================================================================

TEST(MessageCodecTest, UnpackRejectsWrongMagic) {
  std::array<std::byte, 64> buf{};

  // Pack a valid message first.
  std::span<const std::byte> const empty{};
  const auto written = mk::net::pack_message(buf, /*version=*/1, /*msg_type=*/0,
                                             /*flags=*/0, empty);
  ASSERT_NE(written, 0U);

  // Corrupt the first byte of magic.
  buf[0] = std::byte{0xFF};

  mk::net::ParsedMessageView parsed;
  EXPECT_FALSE(mk::net::unpack_message(
      std::span<const std::byte>{buf}.first(written), parsed));
}

// ============================================================================
// 7. Truncated header → unpack returns false
// ============================================================================

TEST(MessageCodecTest, UnpackRejectsTruncatedHeader) {
  // Feed only 12 bytes — not enough for the 16-byte header.
  std::array<std::byte, 12> partial{};
  mk::net::WireWriter w{.buf = std::span<std::byte>{partial}};
  w.write_u32_be(mk::net::kMessageMagic);
  w.write_u16_be(1);
  w.write_u16_be(0);
  // payload_len and flags missing — only 8 bytes of header written.

  mk::net::ParsedMessageView parsed;
  EXPECT_FALSE(mk::net::unpack_message(
      std::span<const std::byte>{partial}.first(w.written()), parsed));
}

// ============================================================================
// 8. payload_len exceeds remaining bytes → unpack returns false
// ============================================================================

TEST(MessageCodecTest, UnpackRejectsPayloadLenExceedsData) {
  std::array<std::byte, 64> buf{};

  // Manually write a header claiming 100 bytes of payload,
  // but only provide the 16-byte header.
  mk::net::WireWriter w{.buf = std::span<std::byte>{buf}};
  w.write_u32_be(mk::net::kMessageMagic);
  w.write_u16_be(1);   // version
  w.write_u16_be(0);   // msg_type
  w.write_u32_be(100); // payload_len = 100
  w.write_u32_be(0);   // flags

  // Pass only header bytes (no actual payload data).
  mk::net::ParsedMessageView parsed;
  EXPECT_FALSE(mk::net::unpack_message(
      std::span<const std::byte>{buf}.first(w.written()), parsed));
}

// ============================================================================
// 9. payload_len exceeds max_payload_size → unpack returns false
// ============================================================================

TEST(MessageCodecTest, UnpackRejectsPayloadLenOverMax) {
  std::array<std::byte, 64> buf{};

  // Header with payload_len = max + 1.
  mk::net::WireWriter w{.buf = std::span<std::byte>{buf}};
  w.write_u32_be(mk::net::kMessageMagic);
  w.write_u16_be(1);
  w.write_u16_be(0);
  w.write_u32_be(static_cast<std::uint32_t>(mk::net::kMaxPayloadSize + 1));
  w.write_u32_be(0);

  mk::net::ParsedMessageView parsed;
  EXPECT_FALSE(mk::net::unpack_message(
      std::span<const std::byte>{buf}.first(w.written()), parsed));
}

// ============================================================================
// 10. Zero-copy: payload span points into original buffer
// ============================================================================

TEST(MessageCodecTest, PayloadIsZeroCopyView) {
  std::array<std::byte, 256> buf{};
  const char text[] = "zero-copy";
  const auto payload = bytes_from(text, 9);

  const auto written = mk::net::pack_message(buf, /*version=*/1, /*msg_type=*/1,
                                             /*flags=*/0, payload);
  ASSERT_NE(written, 0U);

  const auto view = std::span<const std::byte>{buf}.first(written);
  mk::net::ParsedMessageView parsed;
  ASSERT_TRUE(mk::net::unpack_message(view, parsed));

  // The payload data pointer must lie within the original buf.
  const auto *buf_begin = buf.data();
  const auto *buf_end = buf.data() + buf.size();
  EXPECT_GE(parsed.payload.data(), buf_begin);
  EXPECT_LT(parsed.payload.data(), buf_end);

  // Payload should start right after the header.
  const auto *expected_start = reinterpret_cast<const std::byte *>(buf.data()) +
                               mk::net::kMessageHeaderSize;
  EXPECT_EQ(parsed.payload.data(), expected_start);
}

// ============================================================================
// 11. Trailing bytes tolerated by unpack
// ============================================================================

TEST(MessageCodecTest, TrailingBytesTolerated) {
  std::array<std::byte, 256> buf{};
  const char text[] = "msg";
  const auto payload = bytes_from(text, 3);

  const auto written = mk::net::pack_message(buf, /*version=*/1, /*msg_type=*/1,
                                             /*flags=*/0, payload);
  ASSERT_NE(written, 0U);

  // Fill trailing area with garbage.
  std::fill(buf.begin() + static_cast<std::ptrdiff_t>(written), buf.end(),
            std::byte{0xFF});

  // Pass entire 256-byte buffer (message + trailing garbage).
  mk::net::ParsedMessageView parsed;
  ASSERT_TRUE(mk::net::unpack_message(std::span<const std::byte>{buf}, parsed));

  EXPECT_EQ(parsed.header.payload_len, 3U);
  ASSERT_EQ(parsed.payload.size(), 3U);
  EXPECT_EQ(std::memcmp(parsed.payload.data(), text, 3), 0);

  // Caller can compute consumed bytes.
  const auto consumed = static_cast<std::size_t>(
                            parsed.payload.data() -
                            reinterpret_cast<const std::byte *>(buf.data())) +
                        parsed.payload.size();
  EXPECT_EQ(consumed, written);
}
