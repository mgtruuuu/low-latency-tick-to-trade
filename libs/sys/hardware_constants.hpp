/**
 * @file hardware_constants.hpp
 * @brief Hardware and Architecture Constants for Performance Optimization
 *
 * This header defines various constants related to hardware architecture
 * that are crucial for writing high-performance, low-latency code.
 * These constants include cache line sizes, page sizes, SIMD alignment
 * requirements, and double-width CAS (DWCAS) support.
 *
 * Target: Linux x86-64 only.
 *
 * The values are tuned for x86-64 hardware (Intel/AMD). They are essential
 * for optimizing data structures and algorithms to minimize cache misses,
 * false sharing, and TLB misses.
 *
 * Key Sections:
 * 1. Cache Line & Interference Sizes
 * 2. Page Sizes (TLB Optimization)
 * 3. SIMD Alignment Requirements (Vectorization)
 * 4. Double-Width CAS (DWCAS) Constants (128-bit)
 * 5. Pointer Tagging Constants (Virtual Address Space)
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace mk::sys {

// ==============================================================================
// 1. Cache Line & Interference Sizes
// ==============================================================================

// x86-64 (Intel/AMD): cache line size is 64 bytes.
// Both destructive (false-sharing prevention) and constructive (co-location)
// interference sizes are 64 bytes on all current x86-64 processors.
inline constexpr std::size_t kDestructiveInterferenceSize = 64;
inline constexpr std::size_t kConstructiveInterferenceSize = 64;

// General cache line size alias for clarity.
// We typically use 'destructive' size for padding to be safe against false
// sharing.
inline constexpr std::size_t kCacheLineSize = kDestructiveInterferenceSize;

// ==============================================================================
// 2. Page Sizes (TLB Optimization)
// ==============================================================================

// Standard page size (4KB).
inline constexpr std::size_t kPageSize4K = 4096;

// Huge Page (2MB) - Essential for HFT hot paths to reduce TLB misses.
inline constexpr std::size_t kHugePageSize2MB = std::size_t{2} * 1024 * 1024;

// Huge Page (1GB) - Typically used for massive shared memory buffers.
inline constexpr std::size_t kHugePageSize1GB = std::size_t{1024} * 1024 * 1024;

// ==============================================================================
// 3. SIMD Alignment Requirements (Vectorization)
// ==============================================================================

// Alignment required for AVX2 (256-bit) instructions.
inline constexpr std::size_t kAvx2Alignment = 32;

// Alignment required for AVX-512 (512-bit) instructions.
inline constexpr std::size_t kAvx512Alignment = 64;

// ==============================================================================
// 4. Double-Width CAS (DWCAS) Constants (128-bit)
// ==============================================================================

// Alignment required for 128-bit atomic operations (CMPXCHG16B on x86-64).
// Crucial for ABA prevention using {Pointer, Counter} pairs.
inline constexpr std::size_t kDoubleWidthAlignment = 16;

// Check if the compiler supports native 128-bit integers (GCC/Clang extension).
// Always available on GCC/Clang x86-64 Linux.
#if defined(__SIZEOF_INT128__)
using int128_t = __int128_t;
using uint128_t = __uint128_t;
inline constexpr bool kHasInt128 = true;
#else
inline constexpr bool kHasInt128 = false;
#endif

// ==============================================================================
// 5. Pointer Tagging Constants (Virtual Address Space)
// ==============================================================================

// x86-64 uses 48-bit virtual addressing (4-level page tables).
// Bits 48-63 are available for pointer tagging.
inline constexpr std::size_t kVirtualAddressBits = 48;

// The shift required to move data into the high 16 bits.
inline constexpr std::size_t kTagShift = kVirtualAddressBits;

// Mask to extract the raw pointer (lower 48 bits).
// 0x0000FFFFFFFFFFFF
inline constexpr std::uintptr_t kPtrMask =
    (std::uintptr_t(1) << kVirtualAddressBits) - 1;

// Mask to extract the tag (upper 16 bits).
// 0xFFFF000000000000
inline constexpr std::uintptr_t kTagMask = ~kPtrMask;

// ==============================================================================
// 6. Tagging Helper Functions
// ==============================================================================

/**
 * @brief Strips the tag (upper 16 bits) from a pointer.
 * Must be called before dereferencing a tagged pointer.
 * @tparam T The type of the pointer.
 * @param tagged_ptr The pointer containing a tag.
 * @return The raw, dereferenceable pointer.
 */
template <typename T> [[nodiscard]] T *strip_tag(T *tagged_ptr) noexcept {
  auto addr = reinterpret_cast<std::uintptr_t>(tagged_ptr);
  // Apply the mask to clear the upper 16 bits.
  return reinterpret_cast<T *>(addr & kPtrMask);
}

/**
 * @brief Extracts the tag (upper 16 bits) from a pointer.
 * @tparam T The type of the pointer.
 * @param tagged_ptr The pointer containing a tag.
 * @return The 16-bit tag value.
 */
template <typename T>
[[nodiscard]] std::uint16_t get_tag(T *tagged_ptr) noexcept {
  auto addr = reinterpret_cast<std::uintptr_t>(tagged_ptr);
  // Shift right to bring the upper bits to the bottom.
  return static_cast<std::uint16_t>(addr >> kTagShift);
}

} // namespace mk::sys