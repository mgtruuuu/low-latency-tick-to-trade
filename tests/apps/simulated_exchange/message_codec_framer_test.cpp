/**
 * @file message_codec_framer_test.cpp
 * @brief Tests for MessageCodecFramer — TLV frame boundary detection.
 *
 * MessageCodecFramer has a custom contract: decode() returns the FULL TLV
 * frame (header + payload) as the "payload" span, not just the inner payload.
 * These tests fix that contract and guard against regressions.
 */

#include "simulated_exchange/message_codec_framer.hpp"

#include "net/message_codec.hpp"
#include "sys/endian.hpp"

#include <array>
#include <cstddef>
#include <cstring>

#include <gtest/gtest.h>

namespace mk::app {
namespace {

// ---------------------------------------------------------------------------
// Helper: build a valid TLV header in a byte buffer
// ---------------------------------------------------------------------------

void write_header(std::byte *buf, std::uint32_t magic, std::uint16_t version,
                  std::uint16_t msg_type, std::uint32_t payload_len,
                  std::uint32_t flags) noexcept {
  sys::store_be32(buf + 0, magic);
  sys::store_be16(buf + 4, version);
  sys::store_be16(buf + 6, msg_type);
  sys::store_be32(buf + 8, payload_len);
  sys::store_be32(buf + 12, flags);
}

// ---------------------------------------------------------------------------
// decode() tests
// ---------------------------------------------------------------------------

TEST(MessageCodecFramerTest, PartialHeaderReturnsIncomplete) {
  const MessageCodecFramer framer;
  // Less than 16 bytes — not enough for a header.
  std::array<std::byte, 8> buf{};
  const auto result = framer.decode(buf);
  EXPECT_EQ(result.status, net::FrameStatus::kIncomplete);
}

TEST(MessageCodecFramerTest, EmptyBufferReturnsIncomplete) {
  const MessageCodecFramer framer;
  const auto result = framer.decode({});
  EXPECT_EQ(result.status, net::FrameStatus::kIncomplete);
}

TEST(MessageCodecFramerTest, BadMagicReturnsError) {
  const MessageCodecFramer framer;
  std::array<std::byte, 32> buf{};
  write_header(buf.data(), 0xDEADBEEF, 1, 1, 0, 0); // wrong magic
  const auto result = framer.decode(buf);
  EXPECT_EQ(result.status, net::FrameStatus::kError);
}

TEST(MessageCodecFramerTest, OversizedPayloadLenReturnsError) {
  const MessageCodecFramer framer;
  std::array<std::byte, 32> buf{};
  // payload_len > kMaxPayloadSize (65536)
  write_header(buf.data(), net::kMessageMagic, 1, 1, 70000, 0);
  const auto result = framer.decode(buf);
  EXPECT_EQ(result.status, net::FrameStatus::kError);
}

TEST(MessageCodecFramerTest, HeaderOnlyIncompleteWhenPayloadMissing) {
  const MessageCodecFramer framer;
  // Valid header says payload_len=20, but buffer only has 16 bytes (header).
  std::array<std::byte, 16> buf{};
  write_header(buf.data(), net::kMessageMagic, 1, 1, 20, 0);
  const auto result = framer.decode(buf);
  EXPECT_EQ(result.status, net::FrameStatus::kIncomplete);
}

TEST(MessageCodecFramerTest, CompleteFrameReturnsFullTlv) {
  const MessageCodecFramer framer;
  constexpr std::uint32_t kPayloadLen = 8;
  constexpr std::size_t kFrameSize = net::kMessageHeaderSize + kPayloadLen;
  std::array<std::byte, kFrameSize + 4> buf{}; // extra bytes after frame
  write_header(buf.data(), net::kMessageMagic, 1, 42, kPayloadLen, 0);
  // Fill payload with identifiable pattern.
  std::memset(buf.data() + net::kMessageHeaderSize, 0xAB, kPayloadLen);

  const auto result = framer.decode(buf);
  EXPECT_EQ(result.status, net::FrameStatus::kOk);
  EXPECT_EQ(result.frame_size, kFrameSize);
  // Key contract: payload span is the FULL frame (header + payload),
  // not just the inner payload.
  ASSERT_EQ(result.payload.size(), kFrameSize);
  // Verify the payload bytes are the full frame starting from magic.
  EXPECT_EQ(sys::load_be32(result.payload.data()), net::kMessageMagic);
  // Verify msg_type is preserved.
  EXPECT_EQ(sys::load_be16(result.payload.data() + 6), 42U);
}

TEST(MessageCodecFramerTest, ZeroPayloadLenIsValid) {
  const MessageCodecFramer framer;
  std::array<std::byte, net::kMessageHeaderSize> buf{};
  write_header(buf.data(), net::kMessageMagic, 1, 1, 0, 0);
  const auto result = framer.decode(buf);
  EXPECT_EQ(result.status, net::FrameStatus::kOk);
  EXPECT_EQ(result.frame_size, net::kMessageHeaderSize);
  EXPECT_EQ(result.payload.size(), net::kMessageHeaderSize);
}

// ---------------------------------------------------------------------------
// encode() tests
// ---------------------------------------------------------------------------

TEST(MessageCodecFramerTest, EncodePassthroughCopiesExactly) {
  std::array<std::byte, 32> payload{};
  std::memset(payload.data(), 0xCD, payload.size());

  std::array<std::byte, 64> tx_buf{};
  const auto written = MessageCodecFramer::encode(tx_buf, payload);
  EXPECT_EQ(written, payload.size());
  EXPECT_EQ(std::memcmp(tx_buf.data(), payload.data(), payload.size()), 0);
}

TEST(MessageCodecFramerTest, EncodeReturnsZeroWhenBufferTooSmall) {
  std::array<std::byte, 8> small_buf{};
  std::array<std::byte, 32> payload{};
  const auto written = MessageCodecFramer::encode(small_buf, payload);
  EXPECT_EQ(written, 0U);
}

} // namespace
} // namespace mk::app
