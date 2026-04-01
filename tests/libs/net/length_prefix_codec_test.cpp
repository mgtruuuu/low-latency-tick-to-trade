/**
 * @file length_prefix_codec_test.cpp
 * @brief GTest-based tests for length_prefix_codec.hpp.
 *
 * Test plan:
 *   1.  Round-trip: encode → decode, payload matches
 *   2.  Empty payload (header-only frame, total_len == 4)
 *   3.  Max frame size boundary succeeds
 *   4.  Frame exceeds max_frame_size → encode returns nullopt
 *   5.  Output buffer too small → encode returns nullopt
 *   6.  Incomplete header (< 4 bytes) → decode returns kIncomplete
 *   7.  Incomplete payload → decode returns kIncomplete
 *   8.  total_len < 4 → decode returns kError
 *   9.  total_len > max → decode returns kError
 *  10.  Zero-copy: decoded payload points into input buffer
 *  11.  Multiple frames: sequential decode
 *  12.  Trailing bytes tolerated
 *  13.  Custom max_frame_size
 */

#include "net/length_prefix_codec.hpp"
#include "net/wire_writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// ============================================================================
// 1. Round-trip: encode → decode
// ============================================================================

TEST(LengthPrefixCodecTest, RoundTrip) {
  std::array<std::byte, 128> buf{};
  const char text[] = "hello";
  const auto payload =
      std::as_bytes(std::span{text, sizeof(text) - 1}); // 5 bytes

  const auto written = mk::net::encode_length_prefix_frame(buf, payload);
  ASSERT_TRUE(written.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(*written, mk::net::kLengthPrefixSize + 5);

  const auto result =
      mk::net::decode_length_prefix_frame(std::span<const std::byte>{buf}.first(
          *written)); // NOLINT(bugprone-unchecked-optional-access)
  ASSERT_EQ(result.status, mk::net::FrameStatus::kOk);
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(result.frame_size, *written);
  ASSERT_EQ(result.payload.size(), 5U);
  EXPECT_EQ(std::memcmp(result.payload.data(), text, 5), 0);
}

// ============================================================================
// 2. Empty payload
// ============================================================================

TEST(LengthPrefixCodecTest, EmptyPayload) {
  std::array<std::byte, 16> buf{};
  std::span<const std::byte> const empty{};

  const auto written = mk::net::encode_length_prefix_frame(buf, empty);
  ASSERT_TRUE(written.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(*written, mk::net::kLengthPrefixSize);

  const auto result =
      mk::net::decode_length_prefix_frame(std::span<const std::byte>{buf}.first(
          *written)); // NOLINT(bugprone-unchecked-optional-access)
  ASSERT_EQ(result.status, mk::net::FrameStatus::kOk);
  EXPECT_EQ(result.frame_size, mk::net::kLengthPrefixSize);
  EXPECT_TRUE(result.payload.empty());
}

// ============================================================================
// 3. Max frame size boundary
// ============================================================================

TEST(LengthPrefixCodecTest, MaxFrameSizeSucceeds) {
  const std::size_t max_payload =
      mk::net::kDefaultMaxFrameSize - mk::net::kLengthPrefixSize;
  std::vector<std::byte> buf(mk::net::kDefaultMaxFrameSize);
  std::vector<std::byte> payload(max_payload, std::byte{0xAB});

  const auto written = mk::net::encode_length_prefix_frame(buf, payload);
  ASSERT_TRUE(written.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(*written, mk::net::kDefaultMaxFrameSize);

  const auto result =
      mk::net::decode_length_prefix_frame(std::span<const std::byte>{buf}.first(
          *written)); // NOLINT(bugprone-unchecked-optional-access)
  ASSERT_EQ(result.status, mk::net::FrameStatus::kOk);
  EXPECT_EQ(result.payload.size(), max_payload);
}

// ============================================================================
// 4. Frame exceeds max → encode returns nullopt
// ============================================================================

TEST(LengthPrefixCodecTest, EncodeRejectsOversizedPayload) {
  const std::size_t over =
      mk::net::kDefaultMaxFrameSize - mk::net::kLengthPrefixSize + 1;
  std::vector<std::byte> buf(mk::net::kDefaultMaxFrameSize + 64);
  std::vector<std::byte> payload(over, std::byte{0x00});

  const auto written = mk::net::encode_length_prefix_frame(buf, payload);
  EXPECT_FALSE(written.has_value());
}

// ============================================================================
// 5. Output buffer too small → encode returns nullopt
// ============================================================================

TEST(LengthPrefixCodecTest, EncodeRejectsSmallBuffer) {
  // Buffer can't even fit the 4-byte header.
  std::array<std::byte, 2> buf{};
  std::span<const std::byte> const empty{};

  EXPECT_FALSE(mk::net::encode_length_prefix_frame(buf, empty).has_value());
}

TEST(LengthPrefixCodecTest, EncodeRejectsBufferTooSmallForPayload) {
  // Buffer fits header (4) but not header + payload (4 + 8 = 12).
  std::array<std::byte, 8> buf{};
  const std::array<std::byte, 8> payload{};

  EXPECT_FALSE(mk::net::encode_length_prefix_frame(buf, payload).has_value());
}

// ============================================================================
// 6. Incomplete header → decode returns kIncomplete
// ============================================================================

TEST(LengthPrefixCodecTest, DecodeIncompleteHeader) {
  // Only 2 bytes — not enough for the 4-byte header.
  std::array<std::byte, 2> partial{std::byte{0x00}, std::byte{0x08}};

  auto result = mk::net::decode_length_prefix_frame(partial);
  EXPECT_EQ(result.status, mk::net::FrameStatus::kIncomplete);
}

TEST(LengthPrefixCodecTest, DecodeEmptyBuffer) {
  std::span<const std::byte> const empty{};
  auto result = mk::net::decode_length_prefix_frame(empty);
  EXPECT_EQ(result.status, mk::net::FrameStatus::kIncomplete);
}

// ============================================================================
// 7. Incomplete payload → decode returns kIncomplete
// ============================================================================

TEST(LengthPrefixCodecTest, DecodeIncompletePayload) {
  // Header says total_len = 20, but only 10 bytes available.
  std::array<std::byte, 10> buf{};
  mk::net::WireWriter w{.buf = std::span<std::byte>{buf}};
  w.write_u32_be(20); // claims 20 bytes total

  auto result = mk::net::decode_length_prefix_frame(
      std::span<const std::byte>{buf}.first(w.written() + 6));
  EXPECT_EQ(result.status, mk::net::FrameStatus::kIncomplete);
}

// ============================================================================
// 8. total_len < 4 → decode returns kError
// ============================================================================

TEST(LengthPrefixCodecTest, DecodeTotalLenTooSmall) {
  std::array<std::byte, 8> buf{};
  mk::net::WireWriter w{.buf = std::span<std::byte>{buf}};
  w.write_u32_be(3); // less than kLengthPrefixSize

  auto result =
      mk::net::decode_length_prefix_frame(std::span<const std::byte>{buf});
  EXPECT_EQ(result.status, mk::net::FrameStatus::kError);
}

TEST(LengthPrefixCodecTest, DecodeTotalLenZero) {
  std::array<std::byte, 8> buf{};
  mk::net::WireWriter w{.buf = std::span<std::byte>{buf}};
  w.write_u32_be(0);

  auto result =
      mk::net::decode_length_prefix_frame(std::span<const std::byte>{buf});
  EXPECT_EQ(result.status, mk::net::FrameStatus::kError);
}

// ============================================================================
// 9. total_len > max → decode returns kError
// ============================================================================

TEST(LengthPrefixCodecTest, DecodeTotalLenOverMax) {
  std::array<std::byte, 8> buf{};
  mk::net::WireWriter w{.buf = std::span<std::byte>{buf}};
  w.write_u32_be(static_cast<std::uint32_t>(mk::net::kDefaultMaxFrameSize + 1));

  auto result =
      mk::net::decode_length_prefix_frame(std::span<const std::byte>{buf});
  EXPECT_EQ(result.status, mk::net::FrameStatus::kError);
}

// ============================================================================
// 10. Zero-copy: payload points into input buffer
// ============================================================================

TEST(LengthPrefixCodecTest, PayloadIsZeroCopyView) {
  std::array<std::byte, 64> buf{};
  const char text[] = "zero-copy";
  const auto payload = std::as_bytes(std::span{text, 9});

  const auto written = mk::net::encode_length_prefix_frame(buf, payload);
  ASSERT_TRUE(written.has_value());

  const auto view = std::span<const std::byte>{buf}.first(
      *written); // NOLINT(bugprone-unchecked-optional-access)
  const auto result = mk::net::decode_length_prefix_frame(view);
  ASSERT_EQ(result.status, mk::net::FrameStatus::kOk);

  // Payload data pointer must lie within buf, right after the header.
  EXPECT_EQ(result.payload.data(), buf.data() + mk::net::kLengthPrefixSize);
}

// ============================================================================
// 11. Multiple frames: sequential decode
// ============================================================================

TEST(LengthPrefixCodecTest, MultipleFramesSequential) {
  std::array<std::byte, 256> buf{};
  const char msg1[] = "AAA";
  const char msg2[] = "BBBBB";

  // Encode two frames back-to-back.
  auto w1 = mk::net::encode_length_prefix_frame(
      buf, std::as_bytes(std::span{msg1, 3}));
  ASSERT_TRUE(w1.has_value());

  auto w2 = mk::net::encode_length_prefix_frame(
      std::span<std::byte>{buf}.subspan(
          *w1), // NOLINT(bugprone-unchecked-optional-access)
      std::as_bytes(std::span{msg2, 5}));
  ASSERT_TRUE(w2.has_value());

  const std::size_t total =
      *w1 + *w2; // NOLINT(bugprone-unchecked-optional-access)
  auto all = std::span<const std::byte>{buf}.first(total);

  // Decode first frame.
  auto r1 = mk::net::decode_length_prefix_frame(all);
  ASSERT_EQ(r1.status, mk::net::FrameStatus::kOk);
  ASSERT_EQ(r1.payload.size(), 3U);
  EXPECT_EQ(std::memcmp(r1.payload.data(), msg1, 3), 0);

  // Advance past first frame, decode second.
  auto remaining = all.subspan(r1.frame_size);
  auto r2 = mk::net::decode_length_prefix_frame(remaining);
  ASSERT_EQ(r2.status, mk::net::FrameStatus::kOk);
  ASSERT_EQ(r2.payload.size(), 5U);
  EXPECT_EQ(std::memcmp(r2.payload.data(), msg2, 5), 0);

  // No more frames.
  auto tail = remaining.subspan(r2.frame_size);
  EXPECT_TRUE(tail.empty());
}

// ============================================================================
// 12. Trailing bytes tolerated
// ============================================================================

TEST(LengthPrefixCodecTest, TrailingBytesTolerated) {
  std::array<std::byte, 128> buf{};
  const char text[] = "msg";
  const auto payload = std::as_bytes(std::span{text, 3});

  const auto written = mk::net::encode_length_prefix_frame(buf, payload);
  ASSERT_TRUE(written.has_value());

  // Fill trailing area with garbage.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  std::fill(buf.begin() + static_cast<std::ptrdiff_t>(*written), buf.end(),
            std::byte{0xFF});

  // Pass entire buffer (frame + garbage).
  auto result =
      mk::net::decode_length_prefix_frame(std::span<const std::byte>{buf});
  ASSERT_EQ(result.status, mk::net::FrameStatus::kOk);
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(result.frame_size, *written);
  ASSERT_EQ(result.payload.size(), 3U);
  EXPECT_EQ(std::memcmp(result.payload.data(), text, 3), 0);
}

// ============================================================================
// 13. Custom max_frame_size
// ============================================================================

TEST(LengthPrefixCodecTest, CustomMaxFrameSize) {
  constexpr std::size_t kSmallMax = 32;

  // Payload that fits within custom max.
  std::array<std::byte, 64> buf{};
  std::array<std::byte, 20> small_payload{};

  auto written =
      mk::net::encode_length_prefix_frame(buf, small_payload, kSmallMax);
  ASSERT_TRUE(written.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(*written, mk::net::kLengthPrefixSize + 20);

  // Payload that exceeds custom max → rejected.
  std::array<std::byte, 30> big_payload{};
  EXPECT_FALSE(mk::net::encode_length_prefix_frame(buf, big_payload, kSmallMax)
                   .has_value());

  // Decode with the same custom max.
  auto result = mk::net::decode_length_prefix_frame(
      std::span<const std::byte>{buf}.first(
          *written), // NOLINT(bugprone-unchecked-optional-access)
      kSmallMax);
  ASSERT_EQ(result.status, mk::net::FrameStatus::kOk);

  // Decode with a smaller max → kError.
  auto result2 = mk::net::decode_length_prefix_frame(
      std::span<const std::byte>{buf}.first(
          *written), // NOLINT(bugprone-unchecked-optional-access)
      16);
  EXPECT_EQ(result2.status, mk::net::FrameStatus::kError);
}
