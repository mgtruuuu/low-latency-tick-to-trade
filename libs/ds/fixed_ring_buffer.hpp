/**
 * @file fixed_ring_buffer.hpp
 * @brief Fixed-capacity, single-threaded ring buffer with overwrite semantics.
 *
 * Counterpart to RingBuffer<T>: same algorithm and semantics, but capacity is
 * a compile-time value with inline std::array storage.
 *
 * Relationship to RingBuffer:
 *   FixedRingBuffer : RingBuffer  ≈  std::array : std::span
 *   Compile-time capacity → inline std::array storage, constexpr constants.
 *   Runtime capacity → pointer-based storage, runtime constants.
 *
 * When to use each:
 *   FixedRingBuffer — Small, known-size windows (< ~4K elements). Inline
 *     storage avoids indirection and keeps the buffer on the same cache lines
 *     as its owner.
 *   RingBuffer — Large windows or when the size comes from configuration
 *     (e.g., per-instrument moving average window sizes).
 *
 * Designed for sliding-window use cases on the hot path: moving averages,
 * recent-N tick tracking, jitter measurement, etc.
 *
 * Key differences from FixedSPSCQueue:
 *   - Single-threaded (no atomics, no cache-line padding)
 *   - Overwrite semantics: push() overwrites the oldest element when full
 *     (FixedSPSCQueue::try_push() returns false when full)
 *   - Random access: operator[](i) where 0 = oldest, size()-1 = newest
 *   - STL-compatible random-access iterator for algorithm interop
 *
 * Alignment note:
 *   No internal cache-line padding — all members are accessed by the same
 *   thread, so false sharing is impossible. If the FixedRingBuffer instance
 *   itself shares a cache line with other hot data accessed by a different
 *   core, align at the declaration site:
 *     alignas(mk::sys::kCacheLineSize) FixedRingBuffer<double, 64> prices;
 *
 * Internal structure mirrors FixedSPSCQueue: monotonic write index with
 * power-of-two masking for O(1) slot lookup. No modulo anywhere.
 *
 * Template parameters:
 *   T            — Element type (must be trivially destructible, default-
 *                  constructible, nothrow copy- and move-assignable)
 *   CapacityPow2 — Buffer size, must be power-of-two >= 2
 */

#pragma once

#include "sys/bit_utils.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <type_traits>
#include <utility> // std::move

namespace mk::ds {

template <class T, std::size_t CapacityPow2> class FixedRingBuffer {
  static_assert(CapacityPow2 >= 2, "Capacity must be at least 2");
  static_assert(
      mk::sys::is_power_of_two(static_cast<std::uint32_t>(CapacityPow2)),
      "Capacity must be a power of two (for bitwise masking)");
  static_assert(CapacityPow2 <= std::uint32_t(-1),
                "Capacity must fit in uint32_t (indices are 32-bit)");
  // Slots are default-initialized in std::array, so T must be
  // default-constructible.
  static_assert(std::is_default_constructible_v<T>,
                "T must be default-constructible (std::array storage is "
                "default-initialized)");

  static_assert(std::is_nothrow_copy_assignable_v<T>,
                "T must be nothrow copy-assignable (push(const T&) is "
                "noexcept)");
  static_assert(std::is_nothrow_move_assignable_v<T>,
                "T must be nothrow move-assignable (hot-path requirement)");

  // pop_front() and clear() decrement counters without calling destructors.
  // This is safe and correct only for trivially-destructible types —
  // the HFT use case (integers, small POD structs) naturally satisfies this.
  static_assert(std::is_trivially_destructible_v<T>,
                "T must be trivially destructible (pop/clear/overwrite skip "
                "destructors for hot-path performance)");

  // Bitwise mask: index & kMask ∈ [0, CapacityPow2).
  // Replaces expensive modulo (%) with a single AND instruction.
  static constexpr std::uint32_t kMask =
      static_cast<std::uint32_t>(CapacityPow2 - 1U);

  // write_idx_: monotonic index of the next write position.
  //   Wraps naturally at UINT32_MAX. Because CapacityPow2 is a power-of-two
  //   that divides 2^32, the mask produces correct physical indices across
  //   the wrap boundary.
  //
  // count_: number of live elements, in [0, CapacityPow2].
  //   No separate head index needed — head is derived as (write_idx_ - count_).
  std::uint32_t write_idx_{0};
  std::uint32_t count_{0};
  std::array<T, CapacityPow2> buf_{};

  /// Physical index of the oldest element (head).
  /// uint32_t subtraction wraps correctly for monotonic indices.
  [[nodiscard]] std::uint32_t head_physical() const noexcept {
    return (write_idx_ - count_) & kMask;
  }

public:
  // Forward-declare iterator; definition below.
  template <bool IsConst> class IteratorImpl;

  using Iterator = IteratorImpl<false>;
  using ConstIterator = IteratorImpl<true>;

  FixedRingBuffer() = default;
  ~FixedRingBuffer() = default;

  // Non-copyable, non-movable: the inline std::array can be very large.
  // Accidental copies would be extremely expensive.
  FixedRingBuffer(const FixedRingBuffer &) = delete;
  FixedRingBuffer &operator=(const FixedRingBuffer &) = delete;
  FixedRingBuffer(FixedRingBuffer &&) = delete;
  FixedRingBuffer &operator=(FixedRingBuffer &&) = delete;

  // ---------------------------------------------------------------------------
  // Capacity
  // ---------------------------------------------------------------------------

  /// Number of live elements in the buffer.
  [[nodiscard]] std::size_t size() const noexcept { return count_; }

  /// True if the buffer contains no elements.
  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

  /// True if the buffer is at capacity (next push overwrites oldest).
  [[nodiscard]] bool full() const noexcept {
    return count_ == static_cast<std::uint32_t>(CapacityPow2);
  }

  /// Maximum number of elements (compile-time constant).
  [[nodiscard]] static constexpr std::size_t capacity() noexcept {
    return CapacityPow2;
  }

  // ---------------------------------------------------------------------------
  // Element access (0 = oldest, size()-1 = newest)
  // ---------------------------------------------------------------------------

  /// Reference to the oldest element. UB if empty (debug assert).
  [[nodiscard]] T &front() noexcept {
    assert(count_ > 0);
    return buf_[head_physical()];
  }

  [[nodiscard]] const T &front() const noexcept {
    assert(count_ > 0);
    return buf_[head_physical()];
  }

  /// Reference to the newest element. UB if empty (debug assert).
  [[nodiscard]] T &back() noexcept {
    assert(count_ > 0);
    return buf_[(write_idx_ - 1U) & kMask];
  }

  [[nodiscard]] const T &back() const noexcept {
    assert(count_ > 0);
    return buf_[(write_idx_ - 1U) & kMask];
  }

  /// Random access by logical index (0 = oldest).
  /// UB if i >= size() (debug assert).
  [[nodiscard]] T &operator[](std::size_t i) noexcept {
    assert(i < count_);
    return buf_[(head_physical() + static_cast<std::uint32_t>(i)) & kMask];
  }

  [[nodiscard]] const T &operator[](std::size_t i) const noexcept {
    assert(i < count_);
    return buf_[(head_physical() + static_cast<std::uint32_t>(i)) & kMask];
  }

  // ---------------------------------------------------------------------------
  // Modifiers
  // ---------------------------------------------------------------------------

  /// Push a value. If full, the oldest element is overwritten (head advances).
  ///
  /// Overwrite semantics: when count_ == CapacityPow2, the write overwrites
  /// the slot at head_physical(). count_ stays at CapacityPow2 and head
  /// implicitly advances because head_physical() = (write_idx_ - count_).
  void push(const T &value) noexcept {
    buf_[write_idx_ & kMask] = value;
    ++write_idx_;
    if (count_ < static_cast<std::uint32_t>(CapacityPow2)) {
      ++count_;
    }
  }

  /// Move-push variant.
  void push(T &&value) noexcept {
    buf_[write_idx_ & kMask] = std::move(value);
    ++write_idx_;
    if (count_ < static_cast<std::uint32_t>(CapacityPow2)) {
      ++count_;
    }
  }

  /// Remove the oldest element. UB if empty (debug assert).
  /// The slot is not destroyed — next push will overwrite it.
  void pop_front() noexcept {
    assert(count_ > 0);
    --count_;
    // head_physical() automatically advances: count_ decreased while
    // write_idx_ stays the same, so (write_idx_ - count_) increases.
  }

  /// Reset to empty state. O(1) — does not touch element storage.
  void clear() noexcept {
    write_idx_ = 0;
    count_ = 0;
  }

  // ---------------------------------------------------------------------------
  // Iteration
  // ---------------------------------------------------------------------------

  [[nodiscard]] Iterator begin() noexcept { return {this, 0}; }
  [[nodiscard]] Iterator end() noexcept { return {this, count_}; }
  [[nodiscard]] ConstIterator begin() const noexcept { return {this, 0}; }
  [[nodiscard]] ConstIterator end() const noexcept { return {this, count_}; }
  [[nodiscard]] ConstIterator cbegin() const noexcept { return {this, 0}; }
  [[nodiscard]] ConstIterator cend() const noexcept { return {this, count_}; }

  // ===========================================================================
  // Iterator implementation
  // ===========================================================================
  //
  // Random-access iterator over logical indices [0, size()).
  // operator* delegates to FixedRingBuffer::operator[] which handles the
  // physical-index mapping (monotonic index + power-of-two masking).
  //
  // Template parameter IsConst:
  //   false → Iterator  (yields T&)
  //   true  → ConstIterator (const yields T&)

  template <bool IsConst> class IteratorImpl {
    // The enclosing FixedRingBuffer needs to construct iterators (begin/end).
    friend FixedRingBuffer;
    // Allow IteratorImpl<true> to access IteratorImpl<false> privates
    // (for the non-const → const converting constructor).
    template <bool> friend class IteratorImpl;

    using BufferPtr =
        std::conditional_t<IsConst, const FixedRingBuffer *, FixedRingBuffer *>;

    BufferPtr rb_{nullptr};
    std::uint32_t idx_{0};

    IteratorImpl(BufferPtr rb, std::uint32_t idx) noexcept
        : rb_(rb), idx_(idx) {}

  public:
    // STL iterator traits — required for std::iterator_traits and algorithms.
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = std::conditional_t<IsConst, const T *, T *>;
    using reference = std::conditional_t<IsConst, const T &, T &>;

    IteratorImpl() noexcept = default;

    /// Implicit conversion: Iterator → ConstIterator.
    /// Constraint: only enabled when converting non-const to const (not reverse).
    template <bool OtherConst>
      requires(IsConst && !OtherConst)
    // NOLINTNEXTLINE(google-explicit-constructor)
    IteratorImpl(const IteratorImpl<OtherConst> &other) noexcept
        : rb_(other.rb_), idx_(other.idx_) {}

    reference operator*() const noexcept { return (*rb_)[idx_]; }
    pointer operator->() const noexcept { return &(*rb_)[idx_]; }

    reference operator[](difference_type n) const noexcept {
      return (*rb_)[static_cast<std::size_t>(
          static_cast<difference_type>(idx_) + n)];
    }

    // -- Increment / Decrement -----------------------------------------------

    IteratorImpl &operator++() noexcept {
      ++idx_;
      return *this;
    }
    IteratorImpl operator++(int) noexcept {
      auto tmp = *this;
      ++idx_;
      return tmp;
    }
    IteratorImpl &operator--() noexcept {
      --idx_;
      return *this;
    }
    IteratorImpl operator--(int) noexcept {
      auto tmp = *this;
      --idx_;
      return tmp;
    }

    // -- Arithmetic ----------------------------------------------------------

    IteratorImpl &operator+=(difference_type n) noexcept {
      idx_ = static_cast<std::uint32_t>(static_cast<difference_type>(idx_) + n);
      return *this;
    }
    IteratorImpl &operator-=(difference_type n) noexcept {
      idx_ = static_cast<std::uint32_t>(static_cast<difference_type>(idx_) - n);
      return *this;
    }

    friend IteratorImpl operator+(IteratorImpl it, difference_type n) noexcept {
      it += n;
      return it;
    }
    friend IteratorImpl operator+(difference_type n, IteratorImpl it) noexcept {
      it += n;
      return it;
    }
    friend IteratorImpl operator-(IteratorImpl it, difference_type n) noexcept {
      it -= n;
      return it;
    }
    friend difference_type operator-(IteratorImpl a, IteratorImpl b) noexcept {
      return static_cast<difference_type>(a.idx_) -
             static_cast<difference_type>(b.idx_);
    }

    // -- Comparison ----------------------------------------------------------

    friend bool operator==(IteratorImpl a, IteratorImpl b) noexcept {
      return a.idx_ == b.idx_;
    }
    friend auto operator<=>(IteratorImpl a, IteratorImpl b) noexcept {
      return a.idx_ <=> b.idx_;
    }
  };
};

} // namespace mk::ds
