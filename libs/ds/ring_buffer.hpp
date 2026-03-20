/**
 * @file ring_buffer.hpp
 * @brief Runtime-capacity, single-threaded ring buffer with caller-managed
 * storage.
 *
 * Counterpart to FixedRingBuffer<T, N>: same algorithm and semantics, but
 * capacity is a runtime value and the element buffer is provided by the caller
 * (stack array, heap allocation, MmapRegion, shared memory, etc.).
 *
 * Relationship to FixedRingBuffer:
 *   FixedRingBuffer : RingBuffer  ≈  std::array : std::span
 *   Compile-time capacity → inline std::array storage, constexpr constants.
 *   Runtime capacity → pointer-based storage, runtime constants.
 *   Same ring buffer protocol — the logic is independent of where memory lives.
 *
 * When to use each:
 *   FixedRingBuffer — Small, known-size windows (< ~4K elements). Inline
 *     storage avoids indirection and keeps the buffer on the same cache lines
 *     as its owner.
 *   RingBuffer — Large windows or when the size comes from configuration
 *     (e.g., per-instrument moving average window sizes), or when the buffer
 *     must live in a specific memory region (huge pages, NUMA, shared memory).
 *
 * Memory ownership:
 *   RingBuffer NEVER owns memory. The caller allocates and frees the buffer.
 *   This follows the FixedIndexFreeStack principle: operate on external
 * storage, don't own what you index. Use required_buffer_size() and
 *   round_up_capacity() to compute allocation parameters, then pass a void*
 *   buffer to create() or the direct constructor.
 *
 * Design: identical to FixedRingBuffer (monotonic write index, overwrite
 * semantics, power-of-two masking). See fixed_ring_buffer.hpp for detailed
 * algorithm commentary.
 */

#pragma once

#include "sys/bit_utils.hpp" // is_power_of_two, round_up_pow2

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib> // std::abort
#include <iterator>
#include <limits> // std::numeric_limits
#include <optional>
#include <type_traits>
#include <utility> // std::swap, std::move

namespace mk::ds {

template <class T> class RingBuffer {
  // ---------------------------------------------------------------------------
  // Compile-time validation (same constraints as FixedRingBuffer)
  // ---------------------------------------------------------------------------
  static_assert(std::is_trivially_default_constructible_v<T>,
                "T must be trivially default-constructible (external buffer "
                "storage relies on implicit object creation in C++20)");

  static_assert(std::is_nothrow_copy_assignable_v<T>,
                "T must be nothrow copy-assignable (push(const T&) is "
                "noexcept)");
  static_assert(std::is_nothrow_move_assignable_v<T>,
                "T must be nothrow move-assignable (hot-path requirement)");

  // pop_front() and clear() decrement counters without calling destructors.
  // This is safe and correct only for trivially-destructible types.
  static_assert(std::is_trivially_destructible_v<T>,
                "T must be trivially destructible (pop/clear/overwrite skip "
                "destructors for hot-path performance)");

  // ---------------------------------------------------------------------------
  // Data members
  // ---------------------------------------------------------------------------
  T *buf_{nullptr};
  std::uint32_t capacity_{0};
  std::uint32_t mask_{0}; // capacity_ - 1
  std::uint32_t write_idx_{0};
  std::uint32_t count_{0};

  // ---------------------------------------------------------------------------
  // Internal helpers
  // ---------------------------------------------------------------------------

  /// Physical index of the oldest element (head).
  /// uint32_t subtraction wraps correctly for monotonic indices.
  [[nodiscard]] std::uint32_t head_physical() const noexcept {
    return (write_idx_ - count_) & mask_;
  }

  // Private constructor — used by create() factory.
  RingBuffer(T *raw_ptr, std::uint32_t cap) noexcept
      : buf_(raw_ptr), capacity_(cap), mask_(cap - 1) {}

  [[nodiscard]] static constexpr bool
  is_valid_capacity(std::size_t c) noexcept {
    return c >= 2 && mk::sys::is_power_of_two(static_cast<std::uint32_t>(c)) &&
           c <= std::numeric_limits<std::uint32_t>::max();
  }

  static void abort_if_invalid_capacity(std::size_t capacity) noexcept {
    if (!is_valid_capacity(capacity)) {
      std::abort();
    }
  }

public:
  // Forward-declare iterator; definition below.
  template <bool IsConst> class IteratorImpl;

  using Iterator = IteratorImpl<false>;
  using ConstIterator = IteratorImpl<true>;

  // ===========================================================================
  // Static helpers
  // ===========================================================================

  /// Round up to the smallest power-of-two capacity >= n, with minimum 2.
  /// Returns 0 for n == 0 (error).
  [[nodiscard]] static constexpr std::size_t
  round_up_capacity(std::size_t n) noexcept {
    if (n == 0) {
      return 0;
    }
    if (n <= 2) {
      return 2;
    }
    // Capacity is limited to uint32_t range.
    if (n > std::numeric_limits<std::uint32_t>::max()) {
      return 0;
    }
    return mk::sys::round_up_pow2(static_cast<std::uint32_t>(n));
  }

  /// Size of one element in bytes. Needed to allocate external buffers.
  [[nodiscard]] static constexpr std::size_t element_size() noexcept {
    return sizeof(T);
  }

  /// Compute the required buffer size in bytes for a given capacity.
  [[nodiscard]] static constexpr std::size_t
  required_buffer_size(std::size_t capacity) noexcept {
    return capacity * sizeof(T);
  }

  // ===========================================================================
  // Safe Factory (returns std::optional — never aborts)
  // ===========================================================================

  /// Factory — caller supplies raw memory buffer.
  /// The caller is responsible for the buffer's lifetime and deallocation.
  ///
  /// @param external_buf  Pointer to buffer (at least required_buffer_size()
  ///                      bytes, aligned to alignof(T)).
  /// @param buf_size_bytes Size of the external buffer in bytes.
  /// @param capacity       Must be a power of two >= 2. NOT auto-rounded.
  [[nodiscard]] static std::optional<RingBuffer>
  create(void *external_buf, std::size_t buf_size_bytes,
         std::size_t capacity) noexcept {
    if (!is_valid_capacity(capacity) || external_buf == nullptr) {
      return std::nullopt;
    }
    // Misaligned access is UB — reject buffers not aligned to T boundary.
    if (reinterpret_cast<std::uintptr_t>(external_buf) % alignof(T) != 0) {
      return std::nullopt;
    }
    const std::size_t size_in_bytes = capacity * sizeof(T);
    if (buf_size_bytes < size_in_bytes) {
      return std::nullopt;
    }

    return RingBuffer(static_cast<T *>(external_buf),
                      static_cast<std::uint32_t>(capacity));
  }

  // ===========================================================================
  // Direct Constructor (aborts on invalid input — startup-time use)
  // ===========================================================================

  /// Default constructor — creates an empty, unusable buffer (capacity == 0).
  /// Exists to support move assignment patterns.
  RingBuffer() noexcept = default;

  /// Direct constructor — caller supplies the buffer, aborts on invalid input.
  /// Use this at startup time when failure is unrecoverable (abort is
  /// acceptable). Prefer create() when graceful error handling is needed.
  RingBuffer(void *external_buf, std::size_t buf_size_bytes,
             std::size_t capacity) noexcept {
    abort_if_invalid_capacity(capacity);
    if (external_buf == nullptr || buf_size_bytes < capacity * sizeof(T) ||
        reinterpret_cast<std::uintptr_t>(external_buf) % alignof(T) != 0) {
      std::abort();
    }

    capacity_ = static_cast<std::uint32_t>(capacity);
    mask_ = capacity_ - 1;
    buf_ = static_cast<T *>(external_buf);
  }

  // ===========================================================================
  // Move Support (safe before any concurrent use — single-threaded buffer)
  // ===========================================================================

  RingBuffer(RingBuffer &&other) noexcept { swap(other); }

  RingBuffer &operator=(RingBuffer &&other) noexcept {
    RingBuffer tmp(std::move(other));
    swap(tmp);
    return *this;
  }

  // Non-copyable: shallow copy would create aliasing bugs.
  RingBuffer(const RingBuffer &) = delete;
  RingBuffer &operator=(const RingBuffer &) = delete;

  ~RingBuffer() = default;

  void swap(RingBuffer &other) noexcept {
    std::swap(buf_, other.buf_);
    std::swap(capacity_, other.capacity_);
    std::swap(mask_, other.mask_);
    std::swap(write_idx_, other.write_idx_);
    std::swap(count_, other.count_);
  }

  friend void swap(RingBuffer &a, RingBuffer &b) noexcept { a.swap(b); }

  // ===========================================================================
  // Capacity
  // ===========================================================================

  /// Number of live elements in the buffer.
  [[nodiscard]] std::size_t size() const noexcept { return count_; }

  /// True if the buffer contains no elements.
  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

  /// True if the buffer is at capacity (next push overwrites oldest).
  [[nodiscard]] bool full() const noexcept { return count_ == capacity_; }

  /// Maximum number of elements (runtime value).
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  // ===========================================================================
  // Element access (0 = oldest, size()-1 = newest)
  // ===========================================================================

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
    return buf_[(write_idx_ - 1U) & mask_];
  }

  [[nodiscard]] const T &back() const noexcept {
    assert(count_ > 0);
    return buf_[(write_idx_ - 1U) & mask_];
  }

  /// Random access by logical index (0 = oldest).
  /// UB if i >= size() (debug assert).
  [[nodiscard]] T &operator[](std::size_t i) noexcept {
    assert(i < count_);
    return buf_[(head_physical() + static_cast<std::uint32_t>(i)) & mask_];
  }

  [[nodiscard]] const T &operator[](std::size_t i) const noexcept {
    assert(i < count_);
    return buf_[(head_physical() + static_cast<std::uint32_t>(i)) & mask_];
  }

  // ===========================================================================
  // Modifiers
  // ===========================================================================

  /// Push a value. If full, the oldest element is overwritten (head advances).
  /// No-op if capacity == 0 (default-constructed buffer).
  void push(const T &value) noexcept {
    if (capacity_ == 0) [[unlikely]] {
      return;
    }
    buf_[write_idx_ & mask_] = value;
    ++write_idx_;
    if (count_ < capacity_) {
      ++count_;
    }
  }

  /// Move-push variant.
  void push(T &&value) noexcept {
    if (capacity_ == 0) [[unlikely]] {
      return;
    }
    buf_[write_idx_ & mask_] = std::move(value);
    ++write_idx_;
    if (count_ < capacity_) {
      ++count_;
    }
  }

  /// Remove the oldest element. UB if empty (debug assert).
  /// The slot is not destroyed — next push will overwrite it.
  void pop_front() noexcept {
    assert(count_ > 0);
    --count_;
  }

  /// Reset to empty state. O(1) — does not touch element storage.
  void clear() noexcept {
    write_idx_ = 0;
    count_ = 0;
  }

  // ===========================================================================
  // Iteration
  // ===========================================================================

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
  // operator* delegates to RingBuffer::operator[] which handles the
  // physical-index mapping (monotonic index + power-of-two masking).
  //
  // Template parameter IsConst:
  //   false → Iterator  (yields T&)
  //   true  → ConstIterator (const yields T&)

  template <bool IsConst> class IteratorImpl {
    // The enclosing RingBuffer needs to construct iterators (begin/end).
    friend RingBuffer;
    // Allow IteratorImpl<true> to access IteratorImpl<false> privates
    // (for the non-const → const converting constructor).
    template <bool> friend class IteratorImpl;

    using BufferPtr =
        std::conditional_t<IsConst, const RingBuffer *, RingBuffer *>;

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
