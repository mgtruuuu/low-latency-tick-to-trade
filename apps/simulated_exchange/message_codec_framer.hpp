/**
 * @file message_codec_framer.hpp
 * @brief TLV message framer for use with TcpServer.
 *
 * Adapts message_codec.hpp (TLV wire format) to the TcpServer Framer concept.
 *
 * Key design choice: decode() returns the FULL TLV frame (header + payload)
 * as the "payload" span, not just the inner payload. This allows handlers
 * like OrderGateway::on_message() to receive complete TLV messages and
 * parse them independently. The framer's job is purely frame boundary
 * detection — the handler owns message interpretation.
 *
 * Similarly, encode() is a pass-through memcpy because the handler
 * (OrderGateway) already produces fully TLV-wrapped response bytes.
 */

#pragma once

#include "net/length_prefix_codec.hpp" // FrameDecodeResult, FrameStatus
#include "net/message_codec.hpp"       // kMessageHeaderSize, kMessageMagic

#include <cstddef>
#include <cstring>
#include <span>

namespace mk::app {

struct MessageCodecFramer {
  /// Detect a complete TLV message in the receive buffer.
  /// Returns the entire TLV frame (header + payload) as the "payload" span
  /// so the handler can parse the full message including msg_type.
  [[nodiscard]] net::FrameDecodeResult
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  decode(std::span<const std::byte> buf) const noexcept {
    if (buf.size() < net::kMessageHeaderSize) {
      return {.status = net::FrameStatus::kIncomplete, .frame_size = 0, .payload = {}};
    }

    // Peek at magic to distinguish incomplete from corrupt.
    const auto magic = sys::load_be32(buf.data());
    if (magic != net::kMessageMagic) {
      return {.status = net::FrameStatus::kError, .frame_size = 0, .payload = {}};
    }

    // Read payload_len from the header (offset 8, 4 bytes BE).
    const auto plen =
        static_cast<std::size_t>(sys::load_be32(buf.data() + 8));
    if (plen > net::kMaxPayloadSize) {
      return {.status = net::FrameStatus::kError, .frame_size = 0, .payload = {}};
    }

    const std::size_t frame_size = net::kMessageHeaderSize + plen;
    if (buf.size() < frame_size) {
      return {.status = net::FrameStatus::kIncomplete, .frame_size = 0, .payload = {}};
    }

    // Return the entire TLV frame as the "payload" to the handler.
    return {.status = net::FrameStatus::kOk, .frame_size = frame_size, .payload = buf.subspan(0, frame_size)};
  }

  /// Pass-through encode: the handler already produces complete TLV bytes.
  [[nodiscard]] std::size_t
  encode(std::span<std::byte> tx_buf, // NOLINT(readability-convert-member-functions-to-static)
         std::span<const std::byte> payload) const noexcept {
    if (payload.size() > tx_buf.size()) {
      return 0;
    }
    std::memcpy(tx_buf.data(), payload.data(), payload.size());
    return payload.size();
  }
};

} // namespace mk::app
