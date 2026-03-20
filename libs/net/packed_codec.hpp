/**
 * @file packed_codec.hpp
 * @brief Zero-copy codec for packed wire structs (same-endian, no
 * serialization).
 *
 * Alternative to WireWriter/WireReader for same-architecture communication.
 * When both endpoints share the same byte order (e.g., x86-64 ↔ x86-64),
 * packed structs can be memcpy'd directly to/from wire buffers with zero
 * conversion overhead — no per-field serialization, no byte swaps.
 *
 * Usage:
 *   1. Define a #pragma pack(push, 1) struct matching the wire layout.
 *   2. Use pack_struct() / unpack_struct() for type-safe memcpy with
 *      bounds checking.
 *
 * Example:
 *   #pragma pack(push, 1)
 *   struct OrderEntry {
 *     std::uint64_t order_id;
 *     std::int64_t  price;
 *     std::uint32_t qty;
 *     std::uint8_t  side;
 *     // sizeof = 21 (no padding)
 *   };
 *   #pragma pack(pop)
 *   static_assert(sizeof(OrderEntry) == 21);
 *
 *   // Serialize
 *   std::array<std::byte, 64> buf{};
 *   auto n = mk::net::pack_struct(buf, order);  // memcpy, 0 = error
 *
 *   // Deserialize
 *   auto parsed = mk::net::unpack_struct<OrderEntry>(buf);  // optional
 *
 * Comparison with WireWriter/WireReader:
 *
 *   WireWriter/WireReader (field-by-field):
 *     + Endian-independent (works across architectures)
 *     + Flexible (variable-length fields, mixed-endian)
 *     - Per-field store/load + byte swap overhead
 *
 *   packed_codec (zero-copy memcpy):
 *     + Zero serialization overhead (struct IS the wire format)
 *     + Simpler code (no per-message serialize/deserialize functions)
 *     - Both sides must share same byte order
 *     - #pragma pack is compiler extension (not ISO C++, but universally
 *       supported by GCC, Clang, MSVC)
 *
 * HFT context:
 *   Internal firm communication (co-located servers, shared memory IPC)
 *   almost always uses packed structs — both sides are x86-64 Linux,
 *   so endianness and padding are guaranteed identical.
 *   External exchange feeds vary by spec: Nasdaq ITCH uses big-endian
 *   packed structs (still needs byte swap), CME SBE uses little-endian
 *   packed structs (true zero-copy on x86-64).
 */

#pragma once

#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>

namespace mk::net {

/// Concept: a type suitable for zero-copy wire encoding.
/// Must be trivially copyable (safe to memcpy) and standard layout
/// (predictable memory layout across compilers).
template <typename T>
concept PackedWireStruct =
    std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

/// Serialize a packed struct into a buffer via memcpy.
/// No byte-swap, no per-field encoding — the struct bytes ARE the wire bytes.
/// @return Bytes written (sizeof(T)), or 0 if buffer too small.
template <PackedWireStruct T>
[[nodiscard]] std::size_t pack_struct(std::span<std::byte> buf,
                                      const T &msg) noexcept {
  if (buf.size() < sizeof(T)) [[unlikely]] {
    return 0;
  }
  std::memcpy(buf.data(), &msg, sizeof(T));
  return sizeof(T);
}

/// Deserialize a packed struct from a buffer via memcpy.
/// @return The deserialized struct, or nullopt if buffer too small.
///
/// Note: returns by value (std::optional). For hot-path use where you
/// want to avoid copies, use the two-argument overload below.
template <PackedWireStruct T>
[[nodiscard]] std::optional<T>
unpack_struct(std::span<const std::byte> buf) noexcept {
  if (buf.size() < sizeof(T)) [[unlikely]] {
    return std::nullopt;
  }
  T msg{};
  std::memcpy(&msg, buf.data(), sizeof(T));
  return msg;
}

/// Deserialize a packed struct from a buffer into a pre-allocated output.
/// Hot-path variant: avoids std::optional overhead. The caller can
/// pre-allocate the struct and reuse it across iterations.
/// @return true on success, false if buffer too small.
template <PackedWireStruct T>
[[nodiscard]] bool unpack_struct(std::span<const std::byte> buf,
                                 T &out) noexcept {
  if (buf.size() < sizeof(T)) [[unlikely]] {
    return false;
  }
  std::memcpy(&out, buf.data(), sizeof(T));
  return true;
}

} // namespace mk::net
