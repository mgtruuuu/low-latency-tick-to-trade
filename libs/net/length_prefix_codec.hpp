/**
 * @file length_prefix_codec.hpp
 * @brief Length-prefix frame encoder / decoder for streaming TCP protocols.
 *
 * Wire format:
 *   [uint32_t total_len (NBO)] [payload (total_len - 4 bytes)]
 *
 * total_len includes the 4-byte header itself. Minimum valid: 4 (empty
 * payload). Maximum: configurable via max_frame_size parameter.
 *
 * Designed for use with a streaming receive buffer:
 *   1. Accumulate recv() data into a contiguous buffer.
 *   2. Call decode_length_prefix_frame() to check for a complete frame.
 *   3. On kOk, process the payload and advance past frame_size bytes.
 *   4. On kIncomplete, wait for more data.
 *   5. On kError, close the connection (protocol violation).
 *
 * Design:
 *   - Zero allocation, no exceptions — suitable for hot-path use.
 *   - decode returns a non-owning view (payload span points into the
 *     input buffer) for zero-copy processing.
 *   - encode/decode are stateless pure functions — no buffer management.
 */

#pragma once

#include "sys/endian.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace mk::net {

/// Size of the length-prefix header (uint32_t in network byte order).
inline constexpr std::size_t kLengthPrefixSize = 4;

/// Default maximum frame size (header + payload). 64 KiB.
inline constexpr std::size_t kDefaultMaxFrameSize = std::size_t{64} * 1024;

/// Status of a frame decode attempt.
enum class FrameStatus : std::uint8_t {
  kOk,         ///< Complete frame available.
  kIncomplete, ///< Header or payload not fully received yet.
  kError       ///< Invalid frame (total_len < header or > max).
};

/// Result of decode_length_prefix_frame().
/// On kOk, payload is a zero-copy view into the input buffer, and
/// frame_size is the total bytes consumed (header + payload).
struct FrameDecodeResult {
  FrameStatus status{FrameStatus::kIncomplete};
  std::size_t frame_size{0};
  std::span<const std::byte> payload;
};

/// Encodes a length-prefixed frame: [uint32_t total_len (NBO)][payload].
/// total_len = kLengthPrefixSize + payload.size().
/// @return Total bytes written, or nullopt if buffer too small or frame
///         would exceed max_frame_size.
///
/// Single upfront bounds check for the entire frame, then unchecked store.
[[nodiscard]] inline std::optional<std::size_t> encode_length_prefix_frame(
    std::span<std::byte> out, std::span<const std::byte> payload,
    std::size_t max_frame_size = kDefaultMaxFrameSize) noexcept {
  const std::size_t frame_size = kLengthPrefixSize + payload.size();
  if (frame_size > max_frame_size) [[unlikely]] {
    return std::nullopt;
  }
  if (out.size() < frame_size) [[unlikely]] {
    return std::nullopt;
  }

  // Bounds validated — write directly.
  mk::sys::store_be32(out.data(), static_cast<std::uint32_t>(frame_size));
  if (!payload.empty()) {
    std::memcpy(out.data() + kLengthPrefixSize, payload.data(), payload.size());
  }
  return frame_size;
}

/// Attempts to decode one length-prefixed frame from the front of @p buf.
/// Does NOT consume or modify the buffer — the caller advances manually.
///
/// @return FrameDecodeResult with status:
///   - kOk: frame_size and payload are valid.
///   - kIncomplete: need more data (partial header or partial payload).
///   - kError: protocol violation (close the connection).
///
/// Single upfront bounds check for the header, then unchecked load.
[[nodiscard]] inline FrameDecodeResult decode_length_prefix_frame(
    std::span<const std::byte> buf,
    std::size_t max_frame_size = kDefaultMaxFrameSize) noexcept {
  if (buf.size() < kLengthPrefixSize) {
    return {.status = FrameStatus::kIncomplete, .frame_size = 0, .payload = {}};
  }

  // Header bounds validated — read directly.
  const auto total_len =
      static_cast<std::size_t>(mk::sys::load_be32(buf.data()));

  if (total_len < kLengthPrefixSize || total_len > max_frame_size) {
    return {.status = FrameStatus::kError, .frame_size = 0, .payload = {}};
  }

  if (buf.size() < total_len) {
    return {.status = FrameStatus::kIncomplete, .frame_size = 0, .payload = {}};
  }

  const std::size_t payload_len = total_len - kLengthPrefixSize;
  return {.status = FrameStatus::kOk,
          .frame_size = total_len,
          .payload = buf.subspan(kLengthPrefixSize, payload_len)};
}

} // namespace mk::net
