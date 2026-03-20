/**
 * @file wire_reader.hpp
 * @brief Bounds-checked cursor reader for binary wire formats.
 *
 * Reads fixed-width integers from a byte buffer with explicit endian
 * conversion. All reads are bounds-checked; overflow returns std::nullopt
 * without advancing the cursor.
 *
 * Typical usage (parsing a variable-length protocol):
 *
 *   mk::net::WireReader r{received_bytes};
 *   auto magic   = r.read_u32_be();
 *   auto version = r.read_u16_be();
 *   if (!magic || !version) { return parse_error; }
 *
 * Performance note:
 *   Per-field bounds checking + std::optional returns add branch and size
 *   overhead on every read. For fixed-size messages on latency-critical
 *   paths, prefer the single-upfront-bounds-check + unchecked load_be*()
 *   pattern (see protocol_codec.hpp or message_codec.hpp for examples).
 *   WireReader is best suited for:
 *     - Variable-length or unknown-size protocols
 *     - Cold/warm paths where per-field safety outweighs branch cost
 *     - Prototyping and debugging
 *
 * Design:
 *   - Zero allocation, no exceptions.
 *   - Uses memcpy for type-punning (no UB from aliasing or alignment).
 *   - Bounds check uses subtraction form to avoid theoretical overflow
 *     of (pos + size).
 *   - Typed reads return std::optional<T>: nullopt on bounds failure,
 *     mirroring the Writer's bool return for symmetry in error handling.
 *   - std::span<const std::byte> over raw pointer + length (char*, void*):
 *     span carries size, enabling bounds checks; std::byte is not
 *     implicitly convertible, preventing accidental type mixing.
 *     Zero overhead vs raw pointer pair at -O2.
 */

#pragma once

#include "sys/endian.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace mk::net {

struct WireReader {
  std::span<const std::byte> buf;
  std::size_t pos{0};

  // -- Raw bytes --

  /// @brief Reads raw bytes from the buffer into dst.
  /// @return false if not enough data (pos unchanged).
  bool read_bytes(std::span<std::byte> dst) noexcept {
    if (pos > buf.size() || dst.size() > buf.size() - pos) [[unlikely]] {
      return false;
    }
    std::memcpy(dst.data(), buf.data() + pos, dst.size());
    pos += dst.size();
    return true;
  }

  // -- Big-endian (network byte order) --

  std::optional<std::uint16_t> read_u16_be() noexcept {
    std::uint16_t v{};
    if (!read_bytes(std::as_writable_bytes(std::span{&v, 1}))) {
      return std::nullopt;
    }
    return mk::sys::be16_to_host(v);
  }

  std::optional<std::uint32_t> read_u32_be() noexcept {
    std::uint32_t v{};
    if (!read_bytes(std::as_writable_bytes(std::span{&v, 1}))) {
      return std::nullopt;
    }
    return mk::sys::be32_to_host(v);
  }

  std::optional<std::uint64_t> read_u64_be() noexcept {
    std::uint64_t v{};
    if (!read_bytes(std::as_writable_bytes(std::span{&v, 1}))) {
      return std::nullopt;
    }
    return mk::sys::be64_to_host(v);
  }

  // -- Little-endian (IPC / shared memory) --

  std::optional<std::uint16_t> read_u16_le() noexcept {
    std::uint16_t v{};
    if (!read_bytes(std::as_writable_bytes(std::span{&v, 1}))) {
      return std::nullopt;
    }
    return mk::sys::le16_to_host(v);
  }

  std::optional<std::uint32_t> read_u32_le() noexcept {
    std::uint32_t v{};
    if (!read_bytes(std::as_writable_bytes(std::span{&v, 1}))) {
      return std::nullopt;
    }
    return mk::sys::le32_to_host(v);
  }

  std::optional<std::uint64_t> read_u64_le() noexcept {
    std::uint64_t v{};
    if (!read_bytes(std::as_writable_bytes(std::span{&v, 1}))) {
      return std::nullopt;
    }
    return mk::sys::le64_to_host(v);
  }

  // -- Observers --

  /// @brief Number of bytes consumed so far.
  [[nodiscard]] std::size_t consumed() const noexcept { return pos; }

  /// @brief Number of bytes remaining in the buffer.
  [[nodiscard]] std::size_t remaining() const noexcept {
    return buf.size() - pos;
  }
};

} // namespace mk::net
