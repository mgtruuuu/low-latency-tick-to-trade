/**
 * @file fixed_index_free_stack.hpp
 * @brief Fixed-capacity, single-threaded index free stack.
 *
 * Manages a pool of integer indices [0, Capacity) using a LIFO stack.
 * Pop returns the next available index, push returns an index to the pool.
 *
 * This is the simplest possible pool allocator for fixed-size arrays:
 * the caller maps indices to objects via array subscript. Used by OrderBook
 * for Order/PriceLevel slot management, and suitable for any single-threaded
 * component that needs O(1) alloc/free from a fixed set of slots
 * (timer wheels, connection pools, etc.).
 *
 * For concurrent use, see LockFreeStack (CAS-based, in sys/memory/).
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace mk::ds {

template <std::size_t Capacity> class FixedIndexFreeStack {
  static_assert(Capacity > 0, "Capacity must be > 0");
  static_assert(Capacity <= std::numeric_limits<std::uint32_t>::max(),
                "Capacity must fit in uint32_t");

  // No value-initialization (`{}`): the constructor loop writes every element,
  // so zero-init would be redundant work on large pools.
  std::array<std::uint32_t, Capacity> indices_;
  std::uint32_t top_{static_cast<std::uint32_t>(Capacity)};

#ifndef NDEBUG
  // Debug-only allocation tracker: detects double-free and push-without-pop
  // bugs that would silently corrupt the owning pool's slot allocation.
  // Each element tracks whether the corresponding index is currently "in use"
  // (popped but not yet pushed back). Zero overhead in release builds.
  std::array<bool, Capacity> in_use_{};
#endif

public:
  constexpr FixedIndexFreeStack() noexcept {
    // Store indices in reverse so that pop() returns 0, 1, 2, ...
    // This gives sequential forward access into the backing array,
    // which is prefetch-friendly during allocation bursts.
    for (std::uint32_t i = 0; i < Capacity; ++i) {
      indices_[i] = static_cast<std::uint32_t>(Capacity - 1 - i);
    }
  }

  /// Pop the next available index into `out`. Returns false if empty.
  /// Same pattern as SPSCQueue::try_pop() — no std::optional on the hot path.
  [[nodiscard]] constexpr bool pop(std::uint32_t &out) noexcept {
    if (top_ == 0) [[unlikely]] {
      return false;
    }
    out = indices_[--top_];
#ifndef NDEBUG
    in_use_[out] = true;
#endif
    return true;
  }

  /// Return an index to the pool.
  /// Aborts if idx >= Capacity or the stack is already full (push without
  /// a prior pop). These are unrecoverable programmer errors — a corrupt
  /// pool would silently produce invalid indices that cause OOB writes
  /// in the owning data structure. Unlike pop() (graceful false), there
  /// is no meaningful recovery from a bad push.
  /// In Debug builds, additionally aborts on double-free (pushing an index
  /// that is already in the pool) or push-without-pop (pushing an index
  /// that was never allocated). These checks use the in_use_ tracker.
  void push(std::uint32_t idx) noexcept {
    if (idx >= Capacity || top_ >= Capacity) [[unlikely]] {
      std::abort();
    }
#ifndef NDEBUG
    // Catches two classes of bugs:
    //   1. Double-free: pushing an index that was already returned to the pool.
    //   2. Push-without-pop: pushing an index that was never allocated.
    // Both would create duplicate indices, causing the owning data structure
    // (e.g., OrderBook) to hand out the same slot to two objects.
    if (!in_use_[idx]) {
      std::abort();
    }
    in_use_[idx] = false;
#endif
    indices_[top_++] = idx;
  }

  /// Number of indices currently available.
  [[nodiscard]] constexpr std::uint32_t available() const noexcept {
    return top_;
  }

  /// True if no indices are available.
  [[nodiscard]] constexpr bool empty() const noexcept { return top_ == 0; }

  /// True if all indices have been returned (none allocated).
  [[nodiscard]] constexpr bool full() const noexcept {
    return top_ == static_cast<std::uint32_t>(Capacity);
  }

  /// Compile-time capacity.
  [[nodiscard]] static constexpr std::size_t capacity() noexcept {
    return Capacity;
  }
};

} // namespace mk::ds
