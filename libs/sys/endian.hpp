/**
 * @file endian.hpp
 * @brief Endian conversion utilities for portable wire formats.
 *
 * Provides constexpr byte-swap primitives and host/big-endian/little-endian
 * conversion wrappers. Uses C++20 std::endian for compile-time detection —
 * no runtime branches.
 *
 * Design note — intrinsics vs manual:
 *   Production HFT code typically uses compiler intrinsics for byte swaps:
 *     GCC/Clang: __builtin_bswap16/32/64  →  single BSWAP instruction
 *     MSVC:      _byteswap_ushort/ulong/uint64
 *   Intrinsics guarantee a single instruction with predictable latency.
 *   However, intrinsics may not be constexpr on all toolchains.
 *
 *   The manual implementations below are fully constexpr and portable.
 *   Modern compilers (GCC/Clang -O2+) recognize the byte-swap pattern and
 *   emit BSWAP anyway, so runtime performance is equivalent in practice.
 *   If you need guaranteed codegen, replace with intrinsics and drop constexpr.
 */

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mk::sys {

// ==============================================================================
// 1. Byte-Swap Primitives
// ==============================================================================

/// @brief Unconditionally reverses the byte order of a 16-bit value.
[[nodiscard]] constexpr std::uint16_t bswap16(std::uint16_t x) noexcept {
  return static_cast<std::uint16_t>((x >> 8) | (x << 8));
}

/// @brief Unconditionally reverses the byte order of a 32-bit value.
[[nodiscard]] constexpr std::uint32_t bswap32(std::uint32_t x) noexcept {
  return ((x & 0x000000FFU) << 24) | ((x & 0x0000FF00U) << 8) |
         ((x & 0x00FF0000U) >> 8) | ((x & 0xFF000000U) >> 24);
}

/// @brief Unconditionally reverses the byte order of a 64-bit value.
[[nodiscard]] constexpr std::uint64_t bswap64(std::uint64_t x) noexcept {
  return ((x & 0x00000000000000FFULL) << 56) |
         ((x & 0x000000000000FF00ULL) << 40) |
         ((x & 0x0000000000FF0000ULL) << 24) |
         ((x & 0x00000000FF000000ULL) << 8) |
         ((x & 0x000000FF00000000ULL) >> 8) |
         ((x & 0x0000FF0000000000ULL) >> 24) |
         ((x & 0x00FF000000000000ULL) >> 40) |
         ((x & 0xFF00000000000000ULL) >> 56);
}

// ==============================================================================
// 2. Host ↔ Big-Endian (Network Byte Order)
// ==============================================================================
//
// Network protocols (TCP/IP, exchange feeds) conventionally use big-endian.
// On little-endian hosts (x86-64), these swap. On big-endian hosts, no-op.
// The `if constexpr` branch is resolved at compile time — zero runtime cost.

/// @brief Host → big-endian (16-bit).
[[nodiscard]] constexpr std::uint16_t host_to_be16(std::uint16_t x) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    return bswap16(x);
  } else {
    return x;
  }
}

/// @brief Host → big-endian (32-bit).
[[nodiscard]] constexpr std::uint32_t host_to_be32(std::uint32_t x) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    return bswap32(x);
  } else {
    return x;
  }
}

/// @brief Host → big-endian (64-bit).
[[nodiscard]] constexpr std::uint64_t host_to_be64(std::uint64_t x) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    return bswap64(x);
  } else {
    return x;
  }
}

/// @brief Big-endian → host (16-bit). Byte swap is self-inverse.
[[nodiscard]] constexpr std::uint16_t be16_to_host(std::uint16_t x) noexcept {
  return host_to_be16(x);
}

/// @brief Big-endian → host (32-bit). Byte swap is self-inverse.
[[nodiscard]] constexpr std::uint32_t be32_to_host(std::uint32_t x) noexcept {
  return host_to_be32(x);
}

/// @brief Big-endian → host (64-bit). Byte swap is self-inverse.
[[nodiscard]] constexpr std::uint64_t be64_to_host(std::uint64_t x) noexcept {
  return host_to_be64(x);
}

// ==============================================================================
// 3. Host ↔ Little-Endian
// ==============================================================================
//
// Some IPC/shared-memory protocols use little-endian (matches x86 native
// order). On little-endian hosts, these are no-ops. On big-endian hosts, they
// swap.

/// @brief Host → little-endian (16-bit).
[[nodiscard]] constexpr std::uint16_t host_to_le16(std::uint16_t x) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    return bswap16(x);
  } else {
    return x;
  }
}

/// @brief Host → little-endian (32-bit).
[[nodiscard]] constexpr std::uint32_t host_to_le32(std::uint32_t x) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    return bswap32(x);
  } else {
    return x;
  }
}

/// @brief Host → little-endian (64-bit).
[[nodiscard]] constexpr std::uint64_t host_to_le64(std::uint64_t x) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    return bswap64(x);
  } else {
    return x;
  }
}

/// @brief Little-endian → host (16-bit). Byte swap is self-inverse.
[[nodiscard]] constexpr std::uint16_t le16_to_host(std::uint16_t x) noexcept {
  return host_to_le16(x);
}

/// @brief Little-endian → host (32-bit). Byte swap is self-inverse.
[[nodiscard]] constexpr std::uint32_t le32_to_host(std::uint32_t x) noexcept {
  return host_to_le32(x);
}

/// @brief Little-endian → host (64-bit). Byte swap is self-inverse.
[[nodiscard]] constexpr std::uint64_t le64_to_host(std::uint64_t x) noexcept {
  return host_to_le64(x);
}

// ==============================================================================
// 4. Unchecked Load / Store (for pre-validated buffers)
// ==============================================================================
//
// Direct memcpy + endian conversion at a byte pointer. NO bounds checking —
// the caller must ensure sufficient buffer space before calling.
//
// Use case: fixed-size protocol headers where a single upfront bounds check
// replaces per-field checks. Pairs with the conversion functions above.
// Uses memcpy for type-punning (no UB from aliasing or alignment).

inline std::uint16_t load_be16(const std::byte *p) noexcept {
  std::uint16_t v;
  std::memcpy(&v, p, sizeof(v));
  return be16_to_host(v);
}

inline std::uint32_t load_be32(const std::byte *p) noexcept {
  std::uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return be32_to_host(v);
}

inline std::uint64_t load_be64(const std::byte *p) noexcept {
  std::uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return be64_to_host(v);
}

inline void store_be16(std::byte *p, std::uint16_t v) noexcept {
  v = host_to_be16(v);
  std::memcpy(p, &v, sizeof(v));
}

inline void store_be32(std::byte *p, std::uint32_t v) noexcept {
  v = host_to_be32(v);
  std::memcpy(p, &v, sizeof(v));
}

inline void store_be64(std::byte *p, std::uint64_t v) noexcept {
  v = host_to_be64(v);
  std::memcpy(p, &v, sizeof(v));
}

} // namespace mk::sys
