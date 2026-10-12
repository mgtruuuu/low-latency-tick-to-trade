/**
 * @file single_thread_object_pool.hpp
 * @brief Single-threaded, index-based object pool backed by MmapRegion.
 *
 * A concrete, zero-synchronization pool for the per-core hot-path case
 * (thread-per-core / shared-nothing). It is the single-threaded sibling of
 * ConcurrentObjectPool<T> in concurrent_object_pool.hpp:
 *
 *   ConcurrentObjectPool<T>      — MPMC. Wraps each object in a
 *                                  Node { next; T data; } (LockFreeStack node)
 *                                  and recovers the node from a T* via offsetof
 *                                  in deallocate(). 128-bit CAS, ABA-tagged.
 *   SingleThreadObjectPool<T>    — this file. Separate, parallel storage: a raw
 *                                  T[] slot array plus a uint32_t index free
 *                                  list (ds::IndexFreeStack). No per-slot
 *                                  wrapper pointer, no atomics, no CAS, no ABA,
 *                                  no offsetof.
 *
 * Why a separate index stack instead of an embedded union { uint32_t; T }?
 *   The union would save ~4 bytes/slot, but it entangles the object lifetime
 *   with union active-member rules: a free slot's active member is the index,
 *   and returning &slot.data for the caller to field-write would couple "is the
 *   T member's lifetime active yet?" to caller behavior outside the pool. That
 *   is exactly the implicit-lifetime ambiguity that
 *   concurrent_object_pool.hpp's static_assert gate was added to make explicit.
 *   Keeping the free-list metadata in a SEPARATE array means it never overlaps
 *   T storage: a slot is always usable as raw storage for an implicit-lifetime
 *   T, with the same simple lifetime story the pool's T gate validates. (A free
 *   slot may still hold stale T bits; T is trivially destructible, so they are
 *   simply overwritten by the next owner.)
 *
 * Memory layout — one MmapRegion, partitioned into two sections:
 *
 *   [ T slots[capacity] ][ pad ][ IndexFreeStack buffer ]
 *   ^ region base                ^ aligned to alignof(uint32_t)
 *
 * Thread safety: NONE. Caller must ensure single-threaded access. For
 * concurrent (MPMC) use, see ConcurrentObjectPool<T>.
 */

#pragma once

#include "sys/bit_utils.hpp"
#include "sys/hardware_constants.hpp"
#include "sys/log/signal_logger.hpp"
#include "sys/memory/mmap_utils.hpp"

#include "ds/index_free_stack.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib> // std::abort
#include <limits>
#include <type_traits>

namespace mk::sys::memory {

/**
 * @class SingleThreadObjectPool
 * @brief Fixed-size, single-threaded object pool (raw T[] + index free list).
 * @tparam T Object type. Enforced (static_assert): T must be an aggregate AND
 *         trivially destructible — the same implicit-lifetime + destruction
 *         gate as ConcurrentObjectPool's T (see concurrent_object_pool.hpp
 *         for the full standard reasoning). allocate() returns a pointer into
 *         the raw mmap-backed T[] slot array; the caller writes fields directly
 *         with no placement-new, which is only defined for implicit-lifetime
 *         types. Because the free-list metadata lives in a SEPARATE array (not
 *         a Node wrapper over the storage), there is no wrapper/NodeType type
 *         to gate — only T itself.
 */
template <typename T> class SingleThreadObjectPool {
  // Same two safety axes as ConcurrentObjectPool. The detailed standard
  // explanation lives on concurrent_object_pool.hpp; the short version:
  //   - aggregate (+ trivial dtor) => implicit-lifetime, so the raw T[] slots
  //     may be field-written without placement-new.
  //   - trivially destructible     => the pool reuses slots without calling ~T.
  static_assert(std::is_aggregate_v<T>,
                "SingleThreadObjectPool requires T to be an aggregate — a "
                "conservative C++20 sufficient condition for implicit-lifetime "
                "(see concurrent_object_pool.hpp). allocate() field-writes raw "
                "storage without placement-new.");
  static_assert(std::is_trivially_destructible_v<T>,
                "SingleThreadObjectPool requires trivially destructible T "
                "(slots are reused without calling ~T)");
  // The T slot array starts at the MmapRegion base, which is only guaranteed
  // page-aligned (the huge-page path gives 2MB, but the anonymous fallback
  // gives 4K). A T whose alignment exceeds a page could therefore land
  // misaligned at the base, so cap alignof(T) at the 4K page size.
  static_assert(alignof(T) <= kPageSize4K,
                "SingleThreadObjectPool requires alignof(T) <= 4K page size "
                "(the mmap region base is only guaranteed page-aligned)");

public:
  // ========================================================================
  // Layout helpers
  // ========================================================================

  /// Byte offset of the IndexFreeStack section within the region (the T slot
  /// array occupies [0, capacity*sizeof(T)); the index buffer starts at the
  /// next alignof(uint32_t) boundary).
  /// @pre capacity in [1, UINT32_MAX]. The constructors validate this (see
  ///      abort_if_capacity_invalid) before calling; invoking this helper
  ///      directly with a pathological capacity may overflow.
  [[nodiscard]] static constexpr std::size_t
  index_buffer_offset(std::size_t capacity) noexcept {
    return mk::sys::align_up(capacity * sizeof(T), alignof(std::uint32_t));
  }

  /// Total bytes the backing MmapRegion must provide for `capacity` objects:
  /// the T slot array + alignment pad + the index free-list buffer.
  /// Caller-supplied-region constructor callers use this to size their region.
  /// @pre capacity in [1, UINT32_MAX] (same as index_buffer_offset). The
  ///      constructors validate before calling; a pathological capacity passed
  ///      directly may overflow.
  [[nodiscard]] static constexpr std::size_t
  required_region_size(std::size_t capacity) noexcept {
    return index_buffer_offset(capacity) +
           mk::ds::IndexFreeStack::required_buffer_size(capacity);
  }

  // ========================================================================
  // Constructors
  // ========================================================================

  /// Convenience constructor — allocates an MmapRegion (two-level huge-page
  /// fallback) sized for `capacity` objects. Aborts on overflow or allocation
  /// failure (startup-time use).
  /// @param capacity   Number of objects to pre-allocate.
  /// @param pf         Prefault policy (default kPopulateWrite — zero hot-path
  ///                   faults).
  /// @param numa_node  NUMA node to bind to (-1 = no binding).
  /// @param lock_pages Pin pages in RAM (MAP_LOCKED) if true.
  explicit SingleThreadObjectPool(
      std::size_t capacity, PrefaultPolicy pf = PrefaultPolicy::kPopulateWrite,
      int numa_node = -1, bool lock_pages = false)
      : capacity_(capacity) {
    abort_if_capacity_invalid(capacity);

    const std::size_t bytes = required_region_size(capacity);
    const RegionIntentConfig cfg{
        .size = bytes, .numa_node = numa_node, .lock_pages = lock_pages};
    region_ = allocate_region(cfg, to_region_intent(pf));
    init_free_list();
  }

  /// Caller-provided-region constructor — the caller creates the MmapRegion
  /// (any factory: anonymous, huge pages, shared memory, file-backed) and
  /// passes it here, at least required_region_size(capacity) bytes. The pool
  /// takes ownership. Aborts on invalid/too-small region.
  SingleThreadObjectPool(MmapRegion region, std::size_t capacity)
      : capacity_(capacity) {
    if (!region.is_valid()) {
      mk::sys::log::signal_log(
          "[Critical] SingleThreadObjectPool: invalid MmapRegion\n");
      std::abort();
    }
    abort_if_capacity_invalid(capacity);

    const std::size_t bytes = required_region_size(capacity);
    if (region.size() < bytes) {
      mk::sys::log::signal_log(
          "[Critical] SingleThreadObjectPool: MmapRegion too small (need=",
          bytes, ", got=", region.size(), ")\n");
      std::abort();
    }

    region_ = std::move(region);
    init_free_list();
  }

  // Non-copyable, non-movable. The slot pointer (derived from region_.get())
  // and the IndexFreeStack both point into region_'s mapping; the pool owns
  // linked state into externally-mapped memory, so copying or moving would
  // alias or dangle. Deleting move also avoids "moved pool while outstanding
  // slots exist" ambiguity. (Matches the stack policies' semantics.)
  SingleThreadObjectPool(const SingleThreadObjectPool &) = delete;
  SingleThreadObjectPool &operator=(const SingleThreadObjectPool &) = delete;
  SingleThreadObjectPool(SingleThreadObjectPool &&) = delete;
  SingleThreadObjectPool &operator=(SingleThreadObjectPool &&) = delete;
  ~SingleThreadObjectPool() = default;

  // ========================================================================
  // Hot path
  // ========================================================================

  /// Acquire an object slot. Returns nullptr if the pool is exhausted. O(1).
  [[nodiscard]] T *allocate() noexcept {
    std::uint32_t idx = 0;
    if (!free_.pop(idx)) [[unlikely]] {
      return nullptr;
    }
    return slots() + idx;
  }

  /// Return an object slot to the pool. O(1). nullptr is ignored.
  void deallocate(T *obj) noexcept {
    if (obj == nullptr) {
      return;
    }
    // Compute the slot index from the pointer. Integer-address arithmetic
    // (not pointer subtraction) so the debug bounds/alignment check is well-
    // defined even for a pointer that did not originate from this pool.
    const auto raw = reinterpret_cast<std::uintptr_t>(obj);
    const auto base = reinterpret_cast<std::uintptr_t>(slots());
#ifndef NDEBUG
    const auto end =
        base + (static_cast<std::uintptr_t>(capacity_) * sizeof(T));
    assert(raw >= base && raw < end && ((raw - base) % sizeof(T) == 0) &&
           "SingleThreadObjectPool::deallocate: pointer not from this pool");
#endif
    const auto idx = static_cast<std::uint32_t>((raw - base) / sizeof(T));
    free_.push(idx); // IndexFreeStack aborts on out-of-range / double-free
                     // (debug), giving a free double-free guard.
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  /// Exact emptiness (single-threaded — no staleness). Empty means every slot
  /// is currently handed out.
  [[nodiscard]] bool empty() const noexcept { return free_.empty(); }

private:
  /// The raw T[] slot array — the region base. Derived (not stored) so there
  /// is no redundant pointer to keep in sync with region_.
  [[nodiscard]] T *slots() const noexcept {
    return static_cast<T *>(region_.get());
  }

  [[noreturn]] static void abort_invalid_capacity() noexcept {
    mk::sys::log::signal_log(
        "[Critical] SingleThreadObjectPool invalid/overflowing capacity!\n");
    std::abort();
  }

  /// Validate `capacity` and every multiplication/addition that feeds
  /// required_region_size(). Each guard is checked BEFORE the computation it
  /// protects, so no intermediate (including align_up's `+ (align-1)`) can
  /// wrap silently. Aborts on any violation.
  void abort_if_capacity_invalid(std::size_t capacity) const noexcept {
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    constexpr std::size_t kAlign = alignof(std::uint32_t);

    // IndexFreeStack indexes slots with a uint32_t and requires capacity >= 1;
    // a 0 or > UINT32_MAX capacity cannot be represented.
    const bool bad_capacity =
        capacity == 0 ||
        capacity >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    // T slot array bytes (capacity * sizeof(T)) must not overflow.
    const bool slots_overflow = capacity > kMax / sizeof(T);
    if (bad_capacity || slots_overflow) {
      abort_invalid_capacity();
    }

    // align_up(slot_bytes, kAlign) adds (kAlign - 1) before masking, so guard
    // that addition explicitly (the slots_overflow check only bounds
    // slot_bytes <= kMax, not <= kMax - (kAlign - 1)).
    const std::size_t slot_bytes = capacity * sizeof(T);
    if (slot_bytes > kMax - (kAlign - 1)) {
      abort_invalid_capacity();
    }

    // Final partition sum must not overflow. With capacity <= UINT32_MAX the
    // index buffer bytes cannot overflow on their own; only the sum can.
    if (index_buffer_offset(capacity) >
        kMax - mk::ds::IndexFreeStack::required_buffer_size(capacity)) {
      abort_invalid_capacity();
    }
  }

  /// Carve the IndexFreeStack out of the second region partition and fill it
  /// with all `capacity_` indices (every slot initially free).
  void init_free_list() noexcept {
    auto *base = static_cast<char *>(region_.get());
    void *index_buf = base + index_buffer_offset(capacity_);
    free_ = mk::ds::IndexFreeStack(
        index_buf, mk::ds::IndexFreeStack::required_buffer_size(capacity_),
        capacity_);
  }

  std::size_t capacity_;
  MmapRegion region_;           // owns the backing memory (munmap on destruct)
  mk::ds::IndexFreeStack free_; // uint32_t free index stack (2nd partition)
};

} // namespace mk::sys::memory
