/**
 * @file bit_utils.hpp
 * @brief Bitwise math utilities for alignment and power-of-two operations.
 *
 * Pure arithmetic helpers with no hardware-specific constants.
 * Separated from hardware_constants.hpp so that code needing only
 * bit math does not pull in cache-line sizes, page sizes, etc.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <version> // __cpp_lib_* feature-test macros

// C++20 <bit> header: std::has_single_bit, std::bit_ceil.
// __cpp_lib_int_pow2 is the feature-test macro for these specific functions.
// Using the library feature macro (not __cplusplus) because a compiler may
// support C++20 language features while the standard library lags behind.
#if defined(__cpp_lib_int_pow2)
#include <bit>
#endif

namespace mk::sys {

// ==============================================================================
// 1. Alignment Arithmetic
// ==============================================================================

/**
 * @brief Rounds up a value to the nearest multiple of 'alignment'.
 * @param value The value to align.
 * @param alignment Must be a power of 2.
 * @return The aligned value.
 *
 * Bitwise trick:  (value + align-1) & ~(align-1)
 *   align-1    = 0...0 0111  (mask of low bits)
 *   ~(align-1) = 1...1 1000  (clears low bits)
 *   Adding (align-1) first ensures we round UP, not down.
 */
[[nodiscard]] constexpr std::size_t align_up(std::size_t value,
                                             std::size_t alignment) noexcept {
  return (value + alignment - 1) & ~(alignment - 1);
}

/**
 * @brief Rounds down a value to the nearest multiple of 'alignment'.
 * @param value The value to align.
 * @param alignment Must be a power of 2.
 * @return The aligned value (truncated).
 */
[[nodiscard]] constexpr std::size_t align_down(std::size_t value,
                                               std::size_t alignment) noexcept {
  return value & ~(alignment - 1);
}

/**
 * @brief Checks if a pointer (or address) is aligned to the specified boundary.
 * @param addr The address to check.
 * @param alignment Must be a power of 2.
 * @return True if aligned, false otherwise.
 */
[[nodiscard]] constexpr bool is_aligned(std::uintptr_t addr,
                                        std::size_t alignment) noexcept {
  return (addr & (alignment - 1)) == 0;
}

/**
 * @brief Helper to align a raw pointer up to the next boundary.
 * @tparam T Type of the pointer.
 * @param ptr The pointer to align.
 * @param alignment Must be a power of 2.
 * @return Aligned pointer of type T*.
 */
template <typename T>
[[nodiscard]] T *align_ptr_up(T *ptr, std::size_t alignment) noexcept {
  const auto addr = reinterpret_cast<std::uintptr_t>(ptr);
  const auto aligned_addr = align_up(addr, alignment);
  return reinterpret_cast<T *>(aligned_addr);
}

// ==============================================================================
// 2. Power-of-Two Arithmetic
// ==============================================================================

/**
 * @brief Checks whether n is a power of two.
 * Returns false for 0.
 *
 * C++20: delegates to std::has_single_bit (POPCNT or equivalent).
 *
 * Fallback — classic bit trick:  n & (n-1) clears the lowest set bit.
 * A power-of-two has exactly one set bit, so the result is 0.
 * Compiles to BLSR + TEST on x86 (2 instructions).
 */
[[nodiscard]] constexpr bool is_power_of_two(std::size_t n) noexcept {
#if defined(__cpp_lib_int_pow2)
  return std::has_single_bit(n);
#else
  return n != 0 && (n & (n - 1)) == 0;
#endif
}

/**
 * @brief Rounds up to the smallest power-of-two >= n.
 * Returns 1 for n == 0 (matches std::bit_ceil behavior: 2^0 = 1).
 *
 * C++20: delegates to std::bit_ceil (same CLZ-based algorithm, but
 * the standard library version also handles UB for overflow).
 *
 * Fallback — CLZ (Count Leading Zeros) algorithm:
 *   Input: n = 18  (0b 0000...0001 0010)
 *
 *   Step 1: --n → 17  (handle exact powers-of-two: e.g. 16 → 16, not 32)
 *
 *   Step 2: __builtin_clz(17) = 27
 *           CLZ counts the leading zero bits.
 *           17 = 0b 0000...0001 0001  →  27 zeros before the first 1.
 *           On x86 this compiles to LZCNT (1 cycle, no loop).
 *           On ARM this compiles to CLZ   (1 cycle).
 *
 *   Step 3: 32 - 27 = 5
 *           This is the number of bits needed to represent (n-1),
 *           i.e. the bit-position just above the MSB.
 *
 *   Step 4: 1u << 5 = 32
 *           Shift 1 into that position → next power-of-two.
 *
 * Why --n first?
 *   Without it, exact powers-of-two would double:
 *     clz(16) = 27 → 1 << 5 = 32  (wrong, 16 is already valid)
 *     clz(15) = 28 → 1 << 4 = 16  (correct)
 */
[[nodiscard]] constexpr std::uint32_t round_up_pow2(std::uint32_t n) noexcept {
#if defined(__cpp_lib_int_pow2)
  // std::bit_ceil is UB when result would overflow uint32_t.
  // Our original also has UB in that case (shift >= 32), so no change.
  // bit_ceil(0) == 1 (2^0), bit_ceil(1) == 1 — no special-casing needed.
  return std::bit_ceil(n);
#else
  // Guard: __builtin_clz(0) is UB, and n=0 → n-1 wraps to 0xFFFFFFFF
  // → 1u << 32 is UB. Return 1 to match std::bit_ceil behavior.
  if (n <= 1) {
    return 1;
  }
  return 1u << (32u - static_cast<unsigned>(__builtin_clz(n - 1)));
#endif
}

} // namespace mk::sys
