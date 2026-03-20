/**
 * @file message_codec.hpp
 * @brief Length-prefixed message codec with fixed-header wire format.
 *
 * Provides pack_message() and unpack_message() for a fixed-header
 * wire format:
 *
 *   [magic:4][version:2][msg_type:2][payload_len:4][flags:4][payload:N]
 *
 * All multi-byte fields are big-endian (network byte order).
 *
 * Design:
 *   - Zero allocation, no exceptions — suitable for hot-path use.
 *   - unpack_message() populates a non-owning view (ParsedMessageView)
 *     whose payload span points directly into the input buffer (zero-copy).
 *   - Trailing bytes after the message are tolerated — the caller can
 *     compute consumed bytes for streaming parsers.
 *   - Only syntactic validation (magic, bounds). Semantic checks (version
 *     set, msg_type range) are the caller's responsibility.
 */

#pragma once

#include "sys/endian.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace mk::net {

/// Protocol magic number ('SER1' in ASCII).
inline constexpr std::uint32_t kMessageMagic = 0x53455231U;

/// Maximum payload size enforced by the codec (64 KB).
inline constexpr std::size_t kMaxPayloadSize = 65536;

/// Fixed-size header preceding every message on the wire (16 bytes).
struct MessageHeader {
  std::uint32_t magic{};
  std::uint16_t version{};
  std::uint16_t msg_type{};
  std::uint32_t payload_len{};
  std::uint32_t flags{};
};

/// Wire size of MessageHeader in bytes.
inline constexpr std::size_t kMessageHeaderSize =
    sizeof(std::uint32_t) + // magic
    sizeof(std::uint16_t) + // version
    sizeof(std::uint16_t) + // msg_type
    sizeof(std::uint32_t) + // payload_len
    sizeof(std::uint32_t);  // flags
static_assert(kMessageHeaderSize == 16);

static_assert(sizeof(MessageHeader) == kMessageHeaderSize,
              "MessageHeader layout drift — wire size mismatch");

/// Result of unpack_message(). The payload span is a non-owning view into
/// the input buffer — the caller MUST keep that buffer alive and unmodified
/// while accessing payload.
struct ParsedMessageView {
  MessageHeader header{};
  std::span<const std::byte> payload;
};

/// Serializes a message (header + payload) into @p out.
/// @return Total bytes written, or 0 on overflow / payload too large.
///
/// Single upfront bounds check for the entire frame (header + payload),
/// then unchecked stores. Reduces 6 per-field branches to 1.
[[nodiscard]] inline std::size_t
pack_message(std::span<std::byte> out, std::uint16_t version,
             std::uint16_t msg_type, std::uint32_t flags,
             std::span<const std::byte> payload) noexcept {
  if (payload.size() > kMaxPayloadSize) [[unlikely]] {
    return 0;
  }

  const std::size_t total = kMessageHeaderSize + payload.size();
  if (out.size() < total) [[unlikely]] {
    return 0;
  }

  // Bounds validated — write header fields directly.
  auto *p = out.data();
  mk::sys::store_be32(p + 0, kMessageMagic);
  mk::sys::store_be16(p + 4, version);
  mk::sys::store_be16(p + 6, msg_type);
  mk::sys::store_be32(p + 8, static_cast<std::uint32_t>(payload.size()));
  mk::sys::store_be32(p + 12, flags);

  if (!payload.empty()) {
    std::memcpy(p + kMessageHeaderSize, payload.data(), payload.size());
  }
  return total;
}

/// Deserializes one message from the front of @p in into @p out.
/// @return true on success, false on invalid/truncated input (out unchanged).
///
/// Single upfront bounds check for the header, then unchecked loads.
/// Payload bounds are checked separately (variable length).
///
/// Trailing bytes after the message are NOT rejected — a streaming parser
/// can compute consumed bytes:
///   consumed = kMessageHeaderSize + out.header.payload_len;
[[nodiscard]] inline bool unpack_message(std::span<const std::byte> in,
                                         ParsedMessageView &out) noexcept {
  if (in.size() < kMessageHeaderSize) [[unlikely]] {
    return false;
  }

  // Header bounds validated — read fields directly.
  const auto *p = in.data();
  const auto magic = mk::sys::load_be32(p + 0);
  const auto ver = mk::sys::load_be16(p + 4);
  const auto mtype = mk::sys::load_be16(p + 6);
  const auto plen = mk::sys::load_be32(p + 8);
  const auto fl = mk::sys::load_be32(p + 12);

  if (magic != kMessageMagic) [[unlikely]] {
    return false;
  }
  if (plen > kMaxPayloadSize) [[unlikely]] {
    return false;
  }

  const auto payload_len = static_cast<std::size_t>(plen);
  if (payload_len > in.size() - kMessageHeaderSize) [[unlikely]] {
    return false;
  }

  out.header.magic = magic;
  out.header.version = ver;
  out.header.msg_type = mtype;
  out.header.payload_len = plen;
  out.header.flags = fl;
  out.payload = in.subspan(kMessageHeaderSize, payload_len);

  return true;
}

} // namespace mk::net
