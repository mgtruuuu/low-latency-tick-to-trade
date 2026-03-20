/**
 * @file string_pack.hpp
 * @brief Pack short strings (<= 8 bytes) into a uint64_t for O(1) comparison.
 *
 * HFT symbol optimization technique: when an exchange protocol delivers
 * instrument identifiers as short ASCII strings (e.g., FIX Tag 55),
 * packing them into a uint64_t enables:
 *
 *   1. switch/case dispatch on symbol IDs (compiler emits jump table)
 *   2. Single 8-byte comparison instead of memcmp
 *   3. constexpr compile-time constants for known symbols
 *
 * Packing is little-endian: first character occupies the LSB.
 * This matches x86-64 native byte order, so on this platform
 * pack_string() produces the same result as a raw memcpy into uint64_t.
 *
 * Limitation: only useful when the protocol sends raw string symbols.
 * Most modern binary protocols (ITCH, CME MDP3) already use integer
 * instrument IDs — in that case, no packing is needed.
 */

#pragma once

#ifndef MK_SYS_INTERNAL_API
#error "string_pack.hpp is internal. Do not include from public code."
#endif

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib> // std::abort
#include <string_view>

// Enforce little-endian — packing assumes LSB-first byte order.
static_assert(std::endian::native == std::endian::little,
              "pack_string requires little-endian byte order");

namespace mk::sys {

/// Maximum string length that fits in a uint64_t.
inline constexpr std::size_t kMaxPackableStringLen = 8;

/**
 * @brief Pack a string of <= 8 bytes into a uint64_t (little-endian).
 *
 * Returns 0 for empty strings. Aborts if str.size() > 8 — caller must
 * validate at the system boundary before calling.
 *
 * constexpr: can be used to create compile-time symbol constants.
 *
 *   constexpr auto kBtcUsd = pack_string("BTC-USD");
 *   // ... later on hot path:
 *   if (pack_string(incoming_symbol) == kBtcUsd) { ... }
 *
 * Implementation uses explicit shifting rather than memcpy so that
 * the function is constexpr-evaluable. At -O2 the compiler emits
 * a single MOV for known-length inputs.
 */
[[nodiscard]] constexpr std::uint64_t
pack_string(std::string_view str) noexcept {
  if (str.size() > kMaxPackableStringLen) {
    std::abort(); // Precondition violation — not recoverable.
  }

  std::uint64_t result = 0;
  for (std::size_t i = 0; i < str.size(); ++i) {
    result |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(str[i]))
              << (i * 8);
  }
  return result;
}

/**
 * @brief Overload taking raw pointer + length (avoids string_view construction
 *        on hot path when caller already has ptr/len from wire parsing).
 */
[[nodiscard]] inline std::uint64_t pack_string(const char *data,
                                               std::size_t len) noexcept {
  if (len > kMaxPackableStringLen) {
    std::abort();
  }

  std::uint64_t result = 0;
  for (std::size_t i = 0; i < len; ++i) {
    result |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[i]))
              << (i * 8);
  }
  return result;
}

} // namespace mk::sys
