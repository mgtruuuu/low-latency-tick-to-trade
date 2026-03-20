/**
 * @file wire_writer.hpp
 * @brief Bounds-checked cursor writer for binary wire formats.
 *
 * Writes fixed-width integers into a byte buffer with explicit endian
 * conversion. All writes are bounds-checked; overflow returns false
 * without modifying the buffer or advancing the cursor.
 *
 * Typical usage (serializing a variable-length protocol):
 *
 *   std::array<std::byte, 256> buf{};
 *   mk::net::WireWriter w{buf};
 *   if (!w.write_u32_be(magic))   return error;
 *   if (!w.write_u16_be(version)) return error;
 *   if (!w.write_bytes(payload))  return error;
 *   // w.written() == total bytes serialized
 *
 * Performance note:
 *   Per-field bounds checking adds a branch on every write. For fixed-size
 *   messages on latency-critical paths, prefer the single-upfront-bounds-check
 *   + unchecked store_be*() pattern (see protocol_codec.hpp or
 *   message_codec.hpp for examples). WireWriter is best suited for:
 *     - Variable-length or unknown-size protocols
 *     - Cold/warm paths where per-field safety outweighs branch cost
 *     - Prototyping and debugging
 *
 * Design:
 *   - Zero allocation, no exceptions.
 *   - Uses memcpy for type-punning (no UB from aliasing or alignment).
 *   - Bounds check uses subtraction form to avoid theoretical overflow
 *     of (pos + size).
 *   - std::span<std::byte> over raw pointer + length (char*, void*):
 *     span carries size, enabling bounds checks; std::byte is not
 *     implicitly convertible, preventing accidental type mixing.
 *     Zero overhead vs raw pointer pair at -O2.
 */

#pragma once

#include "sys/endian.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace mk::net {

struct WireWriter {
  std::span<std::byte> buf;
  std::size_t pos{0};

  // -- Raw bytes --

  /// @brief Writes raw bytes into the buffer.
  /// @pre src must not be empty — callers are responsible for ensuring
  ///      non-empty data. All internal callers (write_u16_be, etc.)
  ///      satisfy this by construction (fixed sizeof(T) spans).
  /// @return false if not enough space (pos unchanged).
  bool write_bytes(std::span<const std::byte> src) noexcept {
    assert(!src.empty() && "write_bytes: src must not be empty");
    if (pos > buf.size() || src.size() > buf.size() - pos) [[unlikely]] {
      return false;
    }
    std::memcpy(buf.data() + pos, src.data(), src.size());
    pos += src.size();
    return true;
  }

  // -- Big-endian (network byte order) --

  bool write_u16_be(std::uint16_t v) noexcept {
    v = mk::sys::host_to_be16(v);
    return write_bytes(std::as_bytes(std::span{&v, 1}));
  }

  bool write_u32_be(std::uint32_t v) noexcept {
    v = mk::sys::host_to_be32(v);
    return write_bytes(std::as_bytes(std::span{&v, 1}));
  }

  bool write_u64_be(std::uint64_t v) noexcept {
    v = mk::sys::host_to_be64(v);
    return write_bytes(std::as_bytes(std::span{&v, 1}));
  }

  // -- Little-endian (IPC / shared memory) --

  bool write_u16_le(std::uint16_t v) noexcept {
    v = mk::sys::host_to_le16(v);
    return write_bytes(std::as_bytes(std::span{&v, 1}));
  }

  bool write_u32_le(std::uint32_t v) noexcept {
    v = mk::sys::host_to_le32(v);
    return write_bytes(std::as_bytes(std::span{&v, 1}));
  }

  bool write_u64_le(std::uint64_t v) noexcept {
    v = mk::sys::host_to_le64(v);
    return write_bytes(std::as_bytes(std::span{&v, 1}));
  }

  // -- Observers --

  /// @brief Number of bytes written so far.
  [[nodiscard]] std::size_t written() const noexcept { return pos; }

  /// @brief Number of bytes remaining in the buffer.
  [[nodiscard]] std::size_t remaining() const noexcept {
    return buf.size() - pos;
  }
};

} // namespace mk::net
