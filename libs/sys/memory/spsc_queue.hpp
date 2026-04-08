/**
 * @file spsc_queue.hpp
 * @brief Runtime-sized, Lock-free SPSC Ring Buffer (Non-owning).
 *
 * Runtime counterpart to FixedSPSCQueue<T, N>: same SPSC protocol (monotonic
 * indices, Lamport full detection, acquire/release ordering) but the
 * capacity is decided at construction time and the buffer is caller-supplied.
 *
 * Relationship:
 *   FixedSPSCQueue<T, N>  -- compile-time capacity, internal std::array
 *   SPSCQueue<T>           -- runtime capacity, caller-supplied buffer
 *
 * The same non-owning design exists throughout libs/ds/:
 *   RingBuffer<T>     -- runtime capacity, caller-supplied buffer
 *   HashMap<K, V>     -- runtime capacity, caller-supplied buffer
 *   IndexFreeStack    -- runtime capacity, caller-supplied buffer
 *
 * Non-owning design -- caller manages buffer lifetime
 * ---------------------------------------------------
 * SPSCQueue never allocates or frees memory. The caller provides the buffer
 * and is responsible for its lifetime and deallocation. This separates the
 * SPSC protocol from memory ownership, allowing the same code to work on:
 *   - MmapRegion    (huge pages, NUMA-local)
 *   - Shared memory (shm_open + mmap)      -- IPC between processes
 *   - Global/static (global array)         -- zero runtime allocation
 *
 * This is the same separation DPDK rte_ring uses: the ring metadata and
 * the backing memory are independent concerns.
 *
 * The caller is responsible for:
 *   - Ensuring the buffer outlives the SPSCQueue.
 *   - Ensuring the buffer has at least `capacity` elements.
 *   - Freeing the buffer after the SPSCQueue is destroyed.
 */

#pragma once

#include "sys/bit_utils.hpp"
#include "sys/hardware_constants.hpp"
#include "sys/log/signal_logger.hpp"

#include <algorithm> // std::min
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring> // std::memcpy
#include <optional>
#include <type_traits>

namespace mk::sys::memory {

template <class T> class SPSCQueue {
  // Only trivially-copyable, trivially-destructible types are safe to
  // memcpy into a raw buffer without construction/destruction.
  static_assert(std::is_trivially_copyable_v<T>,
                "SPSCQueue requires trivially copyable types");
  static_assert(std::is_trivially_destructible_v<T>,
                "SPSCQueue requires trivially destructible types");

  // ========================================================================
  // Data Members
  // ========================================================================

  // head_ (consumer writes) and tail_ (producer writes) sit on separate
  // cache lines to eliminate false sharing between the two threads.
  //
  // Memory layout (assuming 64-byte cache line):
  //   [--- cache line 0 ---]  head_ (4 bytes) + padding
  //   [--- cache line 1 ---]  tail_ (4 bytes) + padding
  //   buf_raw_ is caller-supplied (MmapRegion, shared memory, etc.).
  alignas(kCacheLineSize) std::atomic<std::uint32_t> head_{0};
  alignas(kCacheLineSize) std::atomic<std::uint32_t> tail_{0};

  std::uint32_t capacity_ = 0; // always a power of two
  std::uint32_t mask_ = 0;     // capacity_ - 1, for fast index wrapping
  T *buf_raw_ = nullptr;       // caller-supplied buffer

public:
  // ========================================================================
  // Static Helpers
  // ========================================================================

  /// Rounds up any positive integer to a valid capacity (power-of-two >= 2).
  /// Returns 0 on invalid input (n == 0) so the caller can detect errors.
  ///
  /// Example:
  ///   round_up_capacity(1000) -> 1024
  ///   round_up_capacity(1)    -> 2    (minimum)
  ///   round_up_capacity(0)    -> 0    (error)
  [[nodiscard]] static constexpr std::uint32_t
  round_up_capacity(std::uint32_t n) noexcept {
    if (n == 0) {
      return 0;
    }
    if (n <= 2) {
      return 2;
    }
    return mk::sys::round_up_pow2(n);
  }

  /// Returns the minimum buffer size (in bytes) required for a given capacity.
  /// Caller should allocate at least this many bytes before constructing.
  ///
  /// Example:
  ///   auto region = MmapRegion::allocate_anonymous(
  ///       SPSCQueue<T>::required_buffer_size(1024));
  [[nodiscard]] static constexpr std::size_t
  required_buffer_size(std::uint32_t capacity) noexcept {
    return static_cast<std::size_t>(capacity) * sizeof(T);
  }

  /// Element alignment requirement. Needed when carving SPSCQueue storage
  /// from a larger contiguous region.
  [[nodiscard]] static constexpr std::size_t element_alignment() noexcept {
    return alignof(T);
  }

  // ========================================================================
  // Safe Factory Function (returns std::optional -- never abort)
  // ========================================================================

  /// Non-owning factory -- validates capacity and buffer, returns nullopt
  /// on failure. Follows the same void* + buf_bytes convention as HashMap,
  /// IndexFreeStack, RingBuffer, and TimingWheel.
  [[nodiscard]] static std::optional<SPSCQueue>
  create(void *external_buf, std::size_t buf_bytes,
         std::uint32_t capacity) noexcept {
    if (!is_valid_capacity(capacity) || external_buf == nullptr) {
      return std::nullopt;
    }
    if (reinterpret_cast<std::uintptr_t>(external_buf) % alignof(T) != 0) {
      return std::nullopt;
    }
    if (buf_bytes < required_buffer_size(capacity)) {
      return std::nullopt;
    }
    return SPSCQueue(external_buf, buf_bytes, capacity);
  }

  // ========================================================================
  // Direct Constructor (aborts on invalid input -- startup-time use)
  // ========================================================================

  /// Non-owning constructor -- caller supplies the buffer as void* + size.
  /// Follows the same convention as HashMap, IndexFreeStack, etc.
  /// Aborts on invalid capacity, null buffer, or insufficient size.
  SPSCQueue(void *external_buf, std::size_t buf_bytes,
            std::uint32_t capacity) noexcept
      : capacity_(capacity), mask_(capacity - 1U),
        buf_raw_(static_cast<T *>(external_buf)) {
    abort_if_invalid_capacity(capacity);
    if (external_buf == nullptr || buf_bytes < required_buffer_size(capacity) ||
        reinterpret_cast<std::uintptr_t>(external_buf) % alignof(T) != 0) {
      std::abort();
    }
  }

  // ========================================================================
  // Move Support (required by std::optional; safe before producer/consumer
  // threads start -- the queue must not be moved while in concurrent use)
  // ========================================================================

  ~SPSCQueue() = default;

  SPSCQueue(SPSCQueue &&other) noexcept { swap(other); }

  SPSCQueue &operator=(SPSCQueue &&other) noexcept {
    SPSCQueue tmp(std::move(other));
    swap(tmp);
    return *this;
  }

  SPSCQueue(const SPSCQueue &) = delete;
  SPSCQueue &operator=(const SPSCQueue &) = delete;

  void swap(SPSCQueue &other) noexcept {
    // Atomics are not std::swap-able -- use explicit load/store.
    // Safe because moves only happen before concurrent use begins.
    auto h = head_.load(std::memory_order_relaxed);
    auto t = tail_.load(std::memory_order_relaxed);
    head_.store(other.head_.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
    tail_.store(other.tail_.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
    other.head_.store(h, std::memory_order_relaxed);
    other.tail_.store(t, std::memory_order_relaxed);

    std::swap(capacity_, other.capacity_);
    std::swap(mask_, other.mask_);
    std::swap(buf_raw_, other.buf_raw_);
  }

  friend void swap(SPSCQueue &a, SPSCQueue &b) noexcept { a.swap(b); }

  // ========================================================================
  // Producer Interface
  // ========================================================================

  /// Producer thread only.
  ///
  /// Memory ordering:
  ///   tail_ relaxed load  -- own variable, only this thread writes it.
  ///   head_ acquire load  -- synchronizes with consumer's release store;
  ///     guarantees the consumer finished reading buf_[slot] before we
  ///     overwrite it.
  ///   tail_ release store -- publishes the data written to buf_[slot]
  ///     so the consumer's acquire load of tail_ will see it.
  [[nodiscard]] bool try_push(const T &v) noexcept {
    const std::uint32_t t = tail_.load(std::memory_order_relaxed);
    const std::uint32_t h = head_.load(std::memory_order_acquire);

    // Lamport-style full detection: uint32_t subtraction wraps correctly
    // because capacity_ (power-of-two) always divides 2^32.
    // All capacity_ slots are usable (no wasted slot).
    if (t - h == capacity_) {
      return false; // full
    }

    buf_raw_[t & mask_] = v;
    tail_.store(t + 1U, std::memory_order_release);
    return true;
  }

  /// Producer thread only: push up to count items with a single release store.
  /// Returns the number of items actually pushed (0 if full).
  ///
  /// Memory ordering:
  ///   Same as try_push, but the release store fires once after ALL writes,
  ///   not once per item. This amortizes the fence cost over N items.
  [[nodiscard]] std::size_t try_push_batch(const T *items,
                                           std::size_t count) noexcept {
    const std::uint32_t t = tail_.load(std::memory_order_relaxed);
    const std::uint32_t h = head_.load(std::memory_order_acquire);
    const std::uint32_t available = capacity_ - (t - h);
    const auto n = static_cast<std::uint32_t>(
        std::min(count, static_cast<std::size_t>(available)));

    for (std::uint32_t i = 0; i < n; ++i) {
      buf_raw_[(t + i) & mask_] = items[i];
    }

    if (n > 0) {
      tail_.store(t + n, std::memory_order_release); // single fence
    }
    return n;
  }

  // ========================================================================
  // Consumer Interface
  // ========================================================================

  /// Consumer thread only.
  ///
  /// Memory ordering:
  ///   head_ relaxed load  -- own variable, only this thread writes it.
  ///   tail_ acquire load  -- synchronizes with producer's release store;
  ///     guarantees the producer finished writing buf_[slot] before we
  ///     read it.
  ///   head_ release store -- publishes that we are done reading buf_[slot]
  ///     so the producer's acquire load of head_ will allow reuse.
  [[nodiscard]] bool try_pop(T &out) noexcept {
    const std::uint32_t h = head_.load(std::memory_order_relaxed);
    const std::uint32_t t = tail_.load(std::memory_order_acquire);
    if (h == t) {
      return false; // empty
    }

    out = buf_raw_[h & mask_];
    head_.store(h + 1U, std::memory_order_release);
    return true;
  }

  /// Consumer thread only: drain up to MaxBatch items with a single release
  /// store. Returns the number of items drained (0 if empty).
  ///
  /// Memory ordering:
  ///   Same as try_pop, but the release store fires once after ALL reads,
  ///   not once per item. This amortizes the fence cost over N items.
  ///   Mirror of try_push_batch on the producer side.
  [[nodiscard]] std::size_t drain(T *out, std::size_t max_count) noexcept {
    assert(out != nullptr && max_count > 0);
    const std::uint32_t h = head_.load(std::memory_order_relaxed);
    const std::uint32_t t = tail_.load(std::memory_order_acquire);
    if (h == t) {
      return 0;
    }
    const auto available = static_cast<std::size_t>(t - h);
    const auto n = std::min(available, max_count);

    // T is guaranteed trivially_copyable (static_assert above), so memcpy
    // is safe and faster than per-element copy assignment -- the compiler
    // can emit rep movsb or SIMD moves for the bulk copy.
    //
    // Ring buffer wrap-around requires at most 2 memcpy calls:
    //   [start .. end-of-buffer] then [0 .. remainder]
    const std::uint32_t start = h & mask_;
    const auto first =
        std::min(static_cast<std::uint32_t>(n), capacity_ - start);
    std::memcpy(out, buf_raw_ + start, first * sizeof(T));
    const auto second = static_cast<std::uint32_t>(n) - first;
    if (second > 0) {
      std::memcpy(out + first, buf_raw_, second * sizeof(T));
    }
    head_.store(h + static_cast<std::uint32_t>(n),
                std::memory_order_release); // single fence
    return n;
  }

  // ========================================================================
  // Observers
  // ========================================================================

  [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

private:
  [[nodiscard]] static constexpr bool
  is_valid_capacity(std::uint32_t c) noexcept {
    return c >= 2 && mk::sys::is_power_of_two(c);
  }

  static void abort_if_invalid_capacity(std::uint32_t capacity) {
    if (!is_valid_capacity(capacity)) {
      mk::sys::log::signal_log(
          "[Critical] SPSCQueue capacity must be power-of-two >= 2\n");
      std::abort();
    }
  }
};

} // namespace mk::sys::memory
