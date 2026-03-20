/**
 * @file aligned_new_allocator.hpp
 * @brief Aligned heap allocator using global new/delete with alignment support.
 *
 * @deprecated In C++20, std::allocator<T> automatically handles over-aligned
 * types (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__). This allocator was
 * necessary in C++17 where std::allocator did not guarantee aligned allocation
 * for over-aligned types. Since this project targets C++20, prefer
 * std::allocator<T> directly. Kept for educational reference only.
 *
 * Original design rationale (C++17):
 * - Initialization and warm-up phases.
 * - Memory-heavy components that require strict alignment.
 * Not recommended for:
 * - Ultra-hot paths where custom allocators (e.g., lock-free pools)
 *   would provide better performance.
 */

#pragma once

#ifndef MK_SYS_INTERNAL_API
#error "memory/aligned_new_allocator.hpp is internal. Do not include from public code."
#endif

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new> // std::align_val_t

namespace mk::sys::memory {
/**
 * @brief Aligned allocator delegating to global heap.
 * @deprecated C++20: use std::allocator<T> instead.
 */
template <class T>
struct
#if __cplusplus >= 202002L
    [[deprecated("C++20: std::allocator handles over-alignment. "
                 "Use std::allocator<T> instead.")]]
#endif
    AlignedNewAllocator {
  using value_type = T;

  AlignedNewAllocator() noexcept = default;

  template <class U>
  AlignedNewAllocator(const AlignedNewAllocator<U> & /*unused*/) noexcept {}

  /**
   * @brief Allocates memory using global operator new.
   * @param n Number of elements of type T to allocate.
   * @return Pointer to the allocated block.
   */
  [[nodiscard]] T *allocate(std::size_t n) {
    // Security Check: Prevent integer overflow
    if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      std::abort();
    }

    const std::size_t bytes = n * sizeof(T);

    // Check if T requires alignment strictly greater than the default.
    if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
      // C++17 aligned allocation: provides both size and alignment hint.
      return static_cast<T *>(
          ::operator new(bytes, std::align_val_t(alignof(T))));
    }

    // Default allocation for standard alignment.
    return static_cast<T *>(::operator new(bytes));
  }

  /**
   * @brief Deallocates memory using global operator delete.
   * @param p Pointer to the memory block.
   */
  void deallocate(T *p, [[maybe_unused]] std::size_t n) noexcept {
    if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
      ::operator delete(p, std::align_val_t(alignof(T)));
    } else {
      ::operator delete(p);
    }
  }

  template <class U>
  bool operator==(const AlignedNewAllocator<U> & /*unused*/) const noexcept {
    return true; // Stateless allocator: all instances are interchangeable
  }
};
} // namespace mk::sys::memory
