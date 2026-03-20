/**
 * @file arena_allocator.hpp
 * @brief Bump-pointer arena allocator and STL-compatible allocator adapter.
 *
 * Arena — Single-block bump-pointer allocator. Two construction modes:
 *   - Non-owning: caller provides external buffer (stack, shared memory).
 *   - Owning: Arena allocates its own MmapRegion (ObjectPool pattern).
 *     Two-level huge page fallback: MAP_HUGETLB → anonymous + MADV_HUGEPAGE.
 *     Configurable PrefaultPolicy, NUMA binding, page locking.
 *
 * HFT memory pattern: all memory is pre-allocated at startup. Arena never
 * grows at runtime — alloc() returns nullptr on OOM (caller decides policy).
 * This matches production HFT systems where auto-growing allocators are
 * forbidden on the hot path.
 *
 * ArenaAllocator<T> — STL-compatible allocator adapter.
 *   OOM policy: std::abort() (no exceptions — HFT hot path safety).
 */

#pragma once

#include "sys/bit_utils.hpp"
#include "sys/log/signal_logger.hpp"
#include "sys/memory/mmap_utils.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdlib> // std::abort (ArenaAllocator OOM)
#include <limits>
#include <span>
#include <type_traits>
#include <utility> // std::swap

namespace mk::sys::memory {

// =============================================================================
// Arena — Single-block bump-pointer allocator
// =============================================================================
//
// NOT thread-safe. Designed for single-threaded use per the HFT thread-per-core
// model: each thread owns its own Arena, pinned to a dedicated core.
// For cross-thread sharing, protect externally or use separate arenas.
//
// Two construction modes (ObjectPool pattern):
//   (1) Non-owning: external buffer via std::span<std::byte>.
//       For stack-allocated buffers (per-message parsing), shared memory, etc.
//   (2) Owning: Arena allocates its own MmapRegion.
//       (a) Convenience ctor — two-level huge page fallback, aborts on failure.
//       (b) MmapRegion ctor — caller controls allocation strategy.
//
// Returns nullptr on OOM (caller decides policy).

class Arena {
  std::byte *base_ = nullptr;
  std::byte *cur_ = nullptr; // Bump pointer (Hot path)
  std::byte *end_ = nullptr;

  // Owning mode: holds the MmapRegion backing store.
  // Default-constructed MmapRegion is invalid (MAP_FAILED) — non-owning mode.
  MmapRegion owned_region_;

public:
  // =========================================================================
  // Constructors
  // =========================================================================

  // (1) Non-owning: external buffer via std::span.
  // [Safety] std::span prevents size/ptr mismatch errors.
  explicit Arena(std::span<std::byte> mem) noexcept
      : base_(mem.data()), cur_(mem.data()), end_(mem.data() + mem.size()) {}

  // (2a) Owning — convenience constructor with two-level huge page fallback.
  // Aborts on mmap failure (unrecoverable, startup-time use).
  //
  // Allocation strategy (cold path, same as ObjectPool):
  //   1. Try MmapRegion::allocate_huge_pages(MAP_HUGETLB) — explicit 2MB pages.
  //   2. Fall back to MmapRegion::allocate_anonymous() + MADV_HUGEPAGE hint.
  //   3. Abort if both fail.
  //
  // @param size       Size in bytes. MmapRegion rounds up to page boundaries.
  // @param pf         Prefault policy (default: kPopulateWrite — zero hot-path
  // faults).
  // @param numa_node  NUMA node to bind memory to (-1 = no binding).
  // @param lock_pages If true, pin pages in RAM (MAP_LOCKED).
  explicit Arena(std::size_t size,
                 PrefaultPolicy pf = PrefaultPolicy::kPopulateWrite,
                 int numa_node = -1, bool lock_pages = false) noexcept {
    assert(size > 0);

    const RegionIntentConfig cfg{
        .size = size, .numa_node = numa_node, .lock_pages = lock_pages};
    RegionIntent intent = RegionIntent::kHotRw; // Defensive default.
    switch (pf) {
    case PrefaultPolicy::kPopulateWrite:
      intent = RegionIntent::kHotRw;
      break;
    case PrefaultPolicy::kPopulateRead:
      intent = RegionIntent::kReadMostly;
      break;
    case PrefaultPolicy::kNone:
      intent = RegionIntent::kCold;
      break;
    case PrefaultPolicy::kManualWrite:
      intent = RegionIntent::kHotRw; // Manual prefault → degrade to hot R/W.
      break;
    case PrefaultPolicy::kManualRead:
      intent = RegionIntent::kReadMostly; // Manual prefault → degrade to read.
      break;
    }
    owned_region_ = allocate_region(cfg, intent);
    base_ = owned_region_.data();
    cur_ = base_;
    end_ = base_ + owned_region_.size();
  }

  // (2b) Owning — caller-provided MmapRegion.
  // The caller creates the MmapRegion using any factory (anonymous, huge pages,
  // shared memory, file-backed) and passes it here. Arena takes ownership.
  // Aborts if the region is invalid.
  explicit Arena(MmapRegion region) noexcept {
    if (!region.is_valid()) {
      mk::sys::log::signal_log("[Critical] Arena: invalid MmapRegion\n");
      std::abort();
    }
    owned_region_ = std::move(region);
    base_ = owned_region_.data();
    cur_ = base_;
    end_ = base_ + owned_region_.size();
  }

  // Non-copyable: two arenas sharing a buffer with independent bump pointers
  // would silently return overlapping memory regions.
  ~Arena() = default;

  Arena(const Arena &) = delete;
  Arena &operator=(const Arena &) = delete;

  // Movable: swap-based (consistent with MmapRegion pattern).
  Arena(Arena &&other) noexcept { swap(other); }
  Arena &operator=(Arena &&other) noexcept {
    Arena tmp(std::move(other));
    swap(tmp);
    return *this;
  }

  void swap(Arena &other) noexcept {
    std::swap(base_, other.base_);
    std::swap(cur_, other.cur_);
    std::swap(end_, other.end_);
    owned_region_.swap(other.owned_region_);
  }
  friend void swap(Arena &a, Arena &b) noexcept { a.swap(b); }

  // --- Observers ---
  [[nodiscard]] std::size_t capacity() const noexcept {
    return static_cast<std::size_t>(end_ - base_);
  }
  [[nodiscard]] std::size_t used() const noexcept {
    return static_cast<std::size_t>(cur_ - base_);
  }
  [[nodiscard]] std::size_t remaining() const noexcept {
    return static_cast<std::size_t>(end_ - cur_);
  }
  [[nodiscard]] bool owns_memory() const noexcept {
    return owned_region_.is_valid();
  }

  // [Performance] Returns aligned pointer using bitwise operations
  [[nodiscard]] void *alloc(std::size_t bytes, std::size_t alignment) noexcept {
    // 1. Min-alignment enforcement (CPU efficiency)
    alignment = std::max(alignment, alignof(void *));

    // 2. alignof(T) is always a power of two per the C++ standard
    //    (§[basic.align]). Violated only by manual misuse, so debug
    //    assert is sufficient — no runtime check on the hot path.
    assert(mk::sys::is_power_of_two(alignment));

    // 3. Align the bump pointer up to the requested boundary.
    auto *p = mk::sys::align_ptr_up(cur_, alignment);

    // 4. Boundary Check (OOM)
    //    Use subtraction (end_ - p) instead of addition (p + bytes) to avoid
    //    pointer arithmetic UB when bytes is larger than the remaining space.
    //    Subtraction between two pointers within the same allocation is always
    //    well-defined; addition past one-past-the-end is not.
    if (std::cmp_greater(bytes, end_ - p)) {
      return nullptr;
    }

    // 5. Bump the pointer
    cur_ = p + bytes;
    return p;
  }

  // Bulk deallocation (Instant)
  void reset() noexcept { cur_ = base_; }
};

// =============================================================================
// ArenaAllocator — STL-compatible allocator adapter
// =============================================================================
//
// Works with Arena. Aborts on OOM (no exceptions — HFT hot path safety).

template <class T> struct ArenaAllocator {
  using value_type = T;
  using propagate_on_container_copy_assignment = std::false_type;
  using propagate_on_container_move_assignment = std::false_type;
  using propagate_on_container_swap = std::false_type;

  // Arena allocators pointing to different arenas are not interchangeable.
  // Explicit false_type documents this (std::allocator_traits would deduce
  // the same, but being explicit shows understanding of the allocator model).
  using is_always_equal = std::false_type;

  Arena *a = nullptr;

  ArenaAllocator() noexcept = default;
  explicit ArenaAllocator(Arena &ar) noexcept : a(&ar) {}

  template <class U>
  ArenaAllocator(const ArenaAllocator<U> &other) noexcept : a(other.a) {}

  [[nodiscard]] T *allocate(std::size_t n) noexcept {
    static_assert(!std::is_const_v<T>);
    if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) [[unlikely]] {
      std::abort();
    }
    const std::size_t bytes = n * sizeof(T);
    void *p = a->alloc(bytes, alignof(T));
    if (p == nullptr) [[unlikely]] {
      std::abort();
    }
    return static_cast<T *>(p);
  }

  void deallocate(T * /*unused*/, std::size_t /*unused*/) noexcept {
    // no-op: arena frees in bulk
  }

  template <class U> struct rebind { // NOLINT(readability-identifier-naming)
    using other = ArenaAllocator<U>;
  };

  // Required so different instantiations compare equal if they share the same
  // arena.
  template <class U>
  bool operator==(const ArenaAllocator<U> &rhs) const noexcept {
    return a == rhs.a;
  }

  template <class U> friend struct ArenaAllocator;
};

} // namespace mk::sys::memory
