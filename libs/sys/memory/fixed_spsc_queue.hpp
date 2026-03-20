/**
 * @file fixed_spsc_queue.hpp
 * @brief Compile-time-sized, Lock-free SPSC Queue (internal std::array).
 *
 * Zero-allocation queue for passing messages between exactly one
 * producer thread and one consumer thread. Uses only atomic load/store
 * (no CAS), making it the fastest possible inter-thread communication.
 *
 * Relationship:
 *   FixedSPSCQueue<T, N>  -- compile-time capacity, internal std::array
 *   SPSCQueue<T>           -- runtime capacity, posix_memalign buffer
 *
 * Key Features:
 * - Monotonic indices with Lamport-style full detection (all slots usable).
 * - Cache-line-padded head/tail to eliminate false sharing.
 * - Capacity must be a power of two (bitwise masking instead of modulo).
 */

#pragma once

#include "sys/bit_utils.hpp"
#include "sys/hardware_constants.hpp"

#include <algorithm> // std::min
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>     // std::memcpy
#include <type_traits> // std::is_trivially_copyable_v

namespace mk::sys::memory {

template <class T, std::size_t CapacityPow2> class FixedSPSCQueue {
  static_assert(CapacityPow2 >= 2, "Capacity must be at least 2");
  static_assert(
      mk::sys::is_power_of_two(static_cast<std::uint32_t>(CapacityPow2)),
      "Capacity must be power-of-two");
  static_assert(CapacityPow2 <= std::uint32_t(-1),
                "Capacity must fit in uint32_t (indices are 32-bit)");

  // Bitwise mask to convert a monotonic index to a buffer position.
  // e.g. CapacityPow2 = 8 -> kMask = 0b0111 -> index & kMask in [0,7]
  // This replaces the expensive modulo (%) with a single AND instruction.
  static constexpr std::uint32_t kMask =
      static_cast<std::uint32_t>(CapacityPow2 - 1U);

  // head_ (consumer writes) and tail_ (producer writes) sit on separate
  // cache lines to eliminate false sharing between the two threads.
  //
  // Memory layout (assuming 64-byte cache line):
  //   [--- cache line 0 ---]  head_ (4 bytes) + 60 bytes padding
  //   [--- cache line 1 ---]  tail_ (4 bytes) + 60 bytes padding
  //   [--- cache line 2+ --]  buf_  (CapacityPow2 * sizeof(T) bytes)
  alignas(kCacheLineSize) std::atomic<std::uint32_t> head_{0};
  alignas(kCacheLineSize) std::atomic<std::uint32_t> tail_{0};
  alignas(kCacheLineSize) std::array<T, CapacityPow2> buf_{};

public:
  FixedSPSCQueue() = default;
  ~FixedSPSCQueue() = default;

  // Atomic members are not copyable/movable.
  FixedSPSCQueue(const FixedSPSCQueue &) = delete;
  FixedSPSCQueue &operator=(const FixedSPSCQueue &) = delete;
  FixedSPSCQueue(FixedSPSCQueue &&) = delete;
  FixedSPSCQueue &operator=(FixedSPSCQueue &&) = delete;

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
    // because CapacityPow2 (power-of-two) always divides 2^32.
    // All CapacityPow2 slots are usable (no wasted slot).
    if (t - h == static_cast<std::uint32_t>(CapacityPow2)) {
      return false; // full
    }

    buf_[t & kMask] = v;
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
    const std::uint32_t available =
        static_cast<std::uint32_t>(CapacityPow2) - (t - h);
    const auto n = static_cast<std::uint32_t>(
        std::min(count, static_cast<std::size_t>(available)));

    for (std::uint32_t i = 0; i < n; ++i) {
      buf_[(t + i) & kMask] = items[i];
    }

    if (n > 0) {
      tail_.store(t + n, std::memory_order_release); // single fence
    }
    return n;
  }

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

    out = buf_[h & kMask];
    head_.store(h + 1U, std::memory_order_release);
    return true;
  }

  /// Consumer thread only: drain up to MaxBatch items with a single release
  /// store. Returns the number of items drained (0 if empty).
  ///
  /// Memory ordering:
  ///   Same as try_pop, but the release store fires once after ALL reads,
  ///   not once per item. This amortizes the fence cost over N items.
  ///
  /// Example (remote-free pattern):
  ///   Thread B (producer): return_q.try_push(handle);
  ///   Thread A (consumer): auto n = return_q.drain(batch);
  ///                        for (size_t i = 0; i < n; ++i)
  ///                        pool.destroy(batch[i]);
  template <std::size_t MaxBatch>
  [[nodiscard]] std::size_t drain(T (&out)[MaxBatch]) noexcept {
    static_assert(MaxBatch > 0, "MaxBatch must be > 0");
    const std::uint32_t h = head_.load(std::memory_order_relaxed);
    const std::uint32_t t = tail_.load(std::memory_order_acquire);
    if (h == t) {
      return 0;
    }
    const auto available = static_cast<std::size_t>(t - h);
    const auto n = std::min(available, MaxBatch);

    if constexpr (std::is_trivially_copyable_v<T>) {
      // Trivially copyable: bulk memcpy is faster than per-element copy
      // assignment — the compiler can emit rep movsb or SIMD moves.
      // Ring buffer wrap-around requires at most 2 memcpy calls:
      //   [start .. end-of-buffer] then [0 .. remainder]
      const std::uint32_t start = h & kMask;
      const auto first = std::min(static_cast<std::uint32_t>(n),
                                  static_cast<std::uint32_t>(CapacityPow2) - start);
      std::memcpy(out, buf_.data() + start, first * sizeof(T));
      const auto second = static_cast<std::uint32_t>(n) - first;
      if (second > 0) {
        std::memcpy(out + first, buf_.data(), second * sizeof(T));
      }
    } else {
      for (std::size_t i = 0; i < n; ++i) {
        out[i] = buf_[(h + static_cast<std::uint32_t>(i)) & kMask];
      }
    }
    head_.store(h + static_cast<std::uint32_t>(n),
                std::memory_order_release); // single fence
    return n;
  }
};

} // namespace mk::sys::memory
