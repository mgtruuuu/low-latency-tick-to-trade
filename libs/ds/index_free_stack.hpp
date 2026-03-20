/**
 * @file index_free_stack.hpp
 * @brief Runtime-capacity, single-threaded index free stack with caller-managed
 * storage.
 *
 * Counterpart to FixedIndexFreeStack<Capacity>: same algorithm and semantics,
 * but capacity is a runtime value and the index buffer is provided by the
 * caller (stack array, heap allocation, MmapRegion, shared memory, etc.).
 *
 * Relationship to FixedIndexFreeStack:
 *   FixedIndexFreeStack : IndexFreeStack  ≈  std::array : std::span
 *   Compile-time capacity → inline std::array storage, constexpr constants.
 *   Runtime capacity → pointer-based storage, runtime constants.
 *   Same LIFO index pool protocol — the logic is independent of where memory
 *   lives.
 *
 * When to use each:
 *   FixedIndexFreeStack — Small, known-size pools (< ~4K slots). Inline
 *     storage avoids indirection and keeps the stack on the same cache lines
 *     as its owner.
 *   IndexFreeStack — Large pools (10K+ slots), or when the buffer must live
 *     in a specific memory region (huge pages, NUMA, shared memory), or when
 *     capacity comes from configuration. Used by TimingWheel (runtime-capacity
 *     timing wheel) for timer node slot management.
 *
 * Memory ownership:
 *   IndexFreeStack NEVER owns memory. The caller allocates and frees the
 *   buffer. Use required_buffer_size() to compute allocation parameters, then
 *   pass a void* buffer to create() or the direct constructor.
 *
 * Design: identical to FixedIndexFreeStack (LIFO stack of uint32_t indices,
 * debug-only in_use_ allocation tracker). See fixed_index_free_stack.hpp for
 * detailed algorithm commentary.
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace mk::ds {

class IndexFreeStack {
  // ===========================================================================
  // Data members
  // ===========================================================================

  std::uint32_t *indices_{nullptr}; // points into external buffer
  std::uint32_t capacity_{0};
  std::uint32_t top_{0};

#ifndef NDEBUG
  // Debug-only allocation tracker — same purpose as
  // FixedIndexFreeStack::in_use_. Lives in the external buffer, after the
  // indices array.
  bool *in_use_{nullptr};
#endif

  // ===========================================================================
  // Validation
  // ===========================================================================

  [[nodiscard]] static bool is_valid_capacity(std::size_t c) noexcept {
    return c >= 1 && c <= std::numeric_limits<std::uint32_t>::max();
  }

  static void abort_if_invalid_capacity(std::size_t c) noexcept {
    if (!is_valid_capacity(c)) {
      std::abort();
    }
  }

  // ===========================================================================
  // Buffer partitioning
  // ===========================================================================

  /// Partition an external buffer into indices_ (and in_use_ in debug).
  /// Returns false if the buffer is too small, null, or misaligned.
  [[nodiscard]] static bool partition_buffer(void *buf, std::size_t buf_bytes,
                                             std::size_t cap,
                                             std::uint32_t *&out_indices
#ifndef NDEBUG
                                             ,
                                             bool *&out_in_use
#endif
                                             ) noexcept {
    if (buf == nullptr) {
      return false;
    }

    // Alignment check: indices are uint32_t.
    if (reinterpret_cast<std::uintptr_t>(buf) % alignof(std::uint32_t) != 0) {
      return false;
    }

    if (buf_bytes < required_buffer_size(cap)) {
      return false;
    }

    out_indices = static_cast<std::uint32_t *>(buf);

#ifndef NDEBUG
    // in_use_ lives right after indices (bool has alignof 1, no padding
    // needed).
    auto *raw = static_cast<std::byte *>(buf);
    out_in_use = reinterpret_cast<bool *>(raw + (cap * sizeof(std::uint32_t)));
#endif

    return true;
  }

  // ===========================================================================
  // Initialization (called after buffer partitioning)
  // ===========================================================================

  void init_storage() noexcept {
    // Store indices in reverse so that pop() returns 0, 1, 2, ...
    // Same pattern as FixedIndexFreeStack constructor.
    for (std::uint32_t i = 0; i < capacity_; ++i) {
      indices_[i] = capacity_ - 1 - i;
    }
    top_ = capacity_;

#ifndef NDEBUG
    for (std::uint32_t i = 0; i < capacity_; ++i) {
      in_use_[i] = false;
    }
#endif
  }

  // ===========================================================================
  // Private constructor (used by create() factory)
  // ===========================================================================

  IndexFreeStack(std::uint32_t *indices,
#ifndef NDEBUG
                 bool *in_use,
#endif
                 std::uint32_t cap) noexcept
      : indices_(indices), capacity_(cap)
#ifndef NDEBUG
        ,
        in_use_(in_use)
#endif
  {
  }

public:
  // ===========================================================================
  // Static helpers
  // ===========================================================================

  /// Compute the required buffer size in bytes for a given capacity.
  /// In debug builds, includes space for the in_use_ tracker.
  [[nodiscard]] static constexpr std::size_t
  required_buffer_size(std::size_t capacity) noexcept {
    std::size_t size = capacity * sizeof(std::uint32_t);
#ifndef NDEBUG
    // bool × capacity for in_use_ tracker (alignof(bool) == 1, no padding).
    size += capacity * sizeof(bool);
#endif
    return size;
  }

  // ===========================================================================
  // Safe Factory (returns std::optional — never aborts)
  // ===========================================================================

  /// Factory — caller supplies raw memory buffer.
  /// The caller is responsible for the buffer's lifetime and deallocation.
  ///
  /// @param external_buf   Pointer to buffer (at least required_buffer_size()
  ///                       bytes, aligned to alignof(uint32_t)).
  /// @param buf_size_bytes Size of the external buffer in bytes.
  /// @param capacity       Number of index slots. Must be >= 1 and fit in
  ///                       uint32_t. Any value is valid (no power-of-two
  ///                       requirement).
  [[nodiscard]] static std::optional<IndexFreeStack>
  create(void *external_buf, std::size_t buf_size_bytes,
         std::size_t capacity) noexcept {
    if (!is_valid_capacity(capacity)) {
      return std::nullopt;
    }

    std::uint32_t *indices = nullptr;
#ifndef NDEBUG
    bool *in_use = nullptr;
#endif

    if (!partition_buffer(external_buf, buf_size_bytes, capacity, indices
#ifndef NDEBUG
                          ,
                          in_use
#endif
                          )) {
      return std::nullopt;
    }

    auto stack = IndexFreeStack(indices,
#ifndef NDEBUG
                                in_use,
#endif
                                static_cast<std::uint32_t>(capacity));
    stack.init_storage();
    return stack;
  }

  // ===========================================================================
  // Direct Constructor (aborts on invalid input — startup-time use)
  // ===========================================================================

  /// Default constructor — empty, unusable (capacity == 0).
  /// Exists to support move assignment patterns.
  IndexFreeStack() noexcept = default;

  /// Direct constructor — caller supplies the buffer, aborts on invalid input.
  /// Use this at startup time when failure is unrecoverable (abort is
  /// acceptable). Prefer create() when graceful error handling is needed.
  IndexFreeStack(void *external_buf, std::size_t buf_size_bytes,
                 std::size_t capacity) noexcept {
    abort_if_invalid_capacity(capacity);

    std::uint32_t *indices = nullptr;
#ifndef NDEBUG
    bool *in_use = nullptr;
#endif

    if (!partition_buffer(external_buf, buf_size_bytes, capacity, indices
#ifndef NDEBUG
                          ,
                          in_use
#endif
                          )) {
      std::abort();
    }

    indices_ = indices;
    capacity_ = static_cast<std::uint32_t>(capacity);
#ifndef NDEBUG
    in_use_ = in_use;
#endif
    init_storage();
  }

  // ===========================================================================
  // Move Support (safe before any concurrent use — single-threaded stack)
  // ===========================================================================

  IndexFreeStack(IndexFreeStack &&other) noexcept { swap(other); }

  IndexFreeStack &operator=(IndexFreeStack &&other) noexcept {
    IndexFreeStack tmp(std::move(other));
    swap(tmp);
    return *this;
  }

  // Non-copyable: shallow copy would create aliasing bugs.
  IndexFreeStack(const IndexFreeStack &) = delete;
  IndexFreeStack &operator=(const IndexFreeStack &) = delete;

  ~IndexFreeStack() = default;

  void swap(IndexFreeStack &other) noexcept {
    std::swap(indices_, other.indices_);
    std::swap(capacity_, other.capacity_);
    std::swap(top_, other.top_);
#ifndef NDEBUG
    std::swap(in_use_, other.in_use_);
#endif
  }

  friend void swap(IndexFreeStack &a, IndexFreeStack &b) noexcept { a.swap(b); }

  // ===========================================================================
  // Core operations (same API as FixedIndexFreeStack)
  // ===========================================================================

  /// Pop the next available index. Returns nullopt if empty.
  [[nodiscard]] std::optional<std::uint32_t> pop() noexcept {
    if (top_ == 0) [[unlikely]] {
      return std::nullopt;
    }
    auto idx = indices_[--top_];
#ifndef NDEBUG
    in_use_[idx] = true;
#endif
    return idx;
  }

  /// Return an index to the pool.
  /// Aborts if idx >= capacity or the stack is already full.
  /// In Debug builds, additionally aborts on double-free or push-without-pop.
  void push(std::uint32_t idx) noexcept {
    if (idx >= capacity_ || top_ >= capacity_) [[unlikely]] {
      std::abort();
    }
#ifndef NDEBUG
    if (!in_use_[idx]) {
      std::abort();
    }
    in_use_[idx] = false;
#endif
    indices_[top_++] = idx;
  }

  // ===========================================================================
  // Observers
  // ===========================================================================

  /// Number of indices currently available.
  [[nodiscard]] std::uint32_t available() const noexcept { return top_; }

  /// True if no indices are available.
  [[nodiscard]] bool empty() const noexcept { return top_ == 0; }

  /// True if all indices have been returned (none allocated).
  [[nodiscard]] bool full() const noexcept { return top_ == capacity_; }

  /// Runtime capacity (number of managed index slots).
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
};

} // namespace mk::ds
