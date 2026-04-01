/**
 * @file object_pool.hpp
 * @brief Fixed-size Object Pool backed by MmapRegion with pluggable free-list.
 *
 * Pre-allocates a fixed number of objects and manages them using a free-list
 * stack whose synchronization strategy is a template parameter (policy-based
 * design).
 *
 * Free-list policies:
 *   - LockFreeStack<T> (default) — MPMC, 128-bit CAS. For cross-thread pools.
 *   - SingleThreadStack<T>       — Zero synchronization. For per-core hot-path
 *                                  pools (the common HFT case).
 *
 * Memory is allocated via MmapRegion with a two-level fallback strategy:
 *   1. MAP_HUGETLB (explicit 2MB huge pages) — best TLB efficiency
 *   2. Anonymous mmap + MADV_HUGEPAGE (transparent huge pages) — fallback
 *
 * Pages are prefaulted at construction (cold path) by default to eliminate
 * first-touch page faults on the hot path.
 */

#pragma once

#include "sys/bit_utils.hpp"
#include "sys/hardware_constants.hpp"
#include "sys/log/signal_logger.hpp"
#include "sys/memory/lock_free_stack.hpp"
#include "sys/memory/mmap_utils.hpp"
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdlib> // std::abort (capacity overflow check)
#include <limits>
#include <type_traits>

namespace mk::sys::memory {

// =============================================================================
// FreeListPolicy concept — contract for ObjectPool free-list policies
// =============================================================================
//
// A free-list must expose:
//   - NodeType       — nested struct with { NodeType* next; T data; }.
//   - push(NodeType*) noexcept — return a node to the pool.
//   - try_pop()       noexcept — acquire a node (nullptr if empty).
//   - empty()         noexcept — emptiness check.
//
// Both LockFreeStack<T> (MPMC) and SingleThreadStack<T> satisfy this.
//
// [Educational note]
// This is a "structural" concept — it checks the interface shape, not
// semantic guarantees. It cannot enforce "push then try_pop returns the
// same node" (that is a runtime property). It CAN enforce that the
// operations exist with correct signatures and noexcept specifiers.
template <typename FL>
concept FreeListPolicy = requires(FL fl, typename FL::NodeType *node) {
  typename FL::NodeType;
  { fl.push(node) } noexcept;
  { fl.try_pop() } noexcept -> std::same_as<typename FL::NodeType *>;
  { fl.empty() } noexcept -> std::same_as<bool>;
};

/**
 * @class ObjectPool
 * @brief Fixed-size Object Pool backed by MmapRegion with pluggable free-list.
 * Features:
 * - Pre-allocated pool of objects to eliminate runtime allocations.
 * - Pluggable free-list policy (LockFreeStack for MPMC, SingleThreadStack
 *   for zero-overhead single-threaded hot paths).
 * - MmapRegion backing: huge pages (MAP_HUGETLB) with automatic fallback.
 * - Configurable prefault policy (default: kPopulateWrite for zero hot-path
 * faults).
 * @tparam T The type of object to pool. Must be trivially destructible
 *           and standard-layout. In practice, T should be an implicit-lifetime
 *           type (see [basic.types.general]: aggregate with non-user-provided
 *           destructor, or has trivial eligible ctor + trivial non-deleted
 *           dtor) so that object
 *           lifetime begins automatically on the backing storage (C++20
 *           P0593R6 for malloc/operator new; this codebase assumes mmap
 *           behaves equivalently on supported toolchains). allocate() returns raw
 *           storage — the caller writes fields directly (no placement-new
 *           needed for implicit-lifetime types).
 * @tparam FreeList The free-list policy. Must satisfy FreeListPolicy concept.
 *           Defaults to LockFreeStack<T> (MPMC, 128-bit CAS).
 *           Use SingleThreadStack<T> for single-threaded hot paths
 *           (zero synchronization, ~20x lower push/pop latency).
 */
template <typename T, FreeListPolicy FreeList = LockFreeStack<T>>
class ObjectPool {
  static_assert(
      std::is_trivially_destructible_v<T>,
      "ObjectPool requires trivially destructible types "
      "(raw memory is returned without constructor/destructor calls)");
  static_assert(
      std::is_standard_layout_v<typename FreeList::NodeType>,
      "NodeType must be standard-layout for offsetof() in deallocate");

public:
  using StackType = FreeList;
  using NodeType = typename FreeList::NodeType;

  // ========================================================================
  // 1. Helper: Calculate Capacity
  // ========================================================================
  /**
   * @brief Calculates optimal capacity to fill Huge Pages.
   * Now simpler because we enforce alignment.
   * @param min_object_count Minimum objects required.
   * @return Capacity aligned to Huge Page size.
   */
  [[nodiscard]] static constexpr std::size_t
  calculate_optimal_capacity(std::size_t min_object_count) noexcept {
    constexpr std::size_t kNodeSize = sizeof(NodeType);
    constexpr std::size_t kHugePage = kHugePageSize2MB;

    // 1. Calculate raw bytes needed (with overflow check)
    if (min_object_count >
        std::numeric_limits<std::size_t>::max() / kNodeSize) {
      return 0; // constexpr context: cannot abort, return 0 to signal error
    }
    const std::size_t bytes_needed = min_object_count * kNodeSize;

    // 2. Align up to exactly 2MB boundary.
    // Guard against unsigned wrap-around in align_up: if bytes_needed is
    // close to SIZE_MAX, adding (kHugePage - 1) would wrap before masking.
    // This can only happen with astronomically large requests, but we
    // handle it for correctness.
    if (bytes_needed >
        std::numeric_limits<std::size_t>::max() - (kHugePage - 1)) {
      return 0;
    }
    const std::size_t total_chunk_size = mk::sys::align_up(bytes_needed, kHugePage);

    return total_chunk_size / kNodeSize;
  }

  // ========================================================================
  // 2. Constructor
  // ========================================================================
  //
  // Allocation strategy (cold path):
  //   1. Try MmapRegion::allocate_huge_pages(MAP_HUGETLB) — explicit 2MB pages.
  //      Best TLB performance but requires system configuration
  //      (/proc/sys/vm/nr_hugepages or hugetlbfs).
  //   2. Fall back to MmapRegion::allocate_anonymous() + MADV_HUGEPAGE hint.
  //      The kernel's transparent huge page (THP) daemon may promote these
  //      to huge pages opportunistically — no sysadmin setup required.
  //   3. Abort if both fail (memory exhaustion is unrecoverable for a pool).
  //
  // PrefaultPolicy default: kPopulateWrite — prefaults all pages during
  // construction so the hot path never takes a first-touch page fault.
  // Use kNone if you want lazy population (e.g., for benchmarking setup time).
  //
  /// @param capacity   Number of objects to pre-allocate.
  /// @param pf         Prefault policy (default: kPopulateWrite — zero hot-path
  /// faults).
  /// @param numa_node  NUMA node to bind memory to (-1 = no binding, use OS
  /// default).
  ///   On dual-socket HFT servers, bind to the NIC-local node to avoid
  ///   cross-socket QPI/UPI traffic. On single-socket systems, leave as -1.
  /// @param lock_pages If true, pin pages in RAM (prevent swapout via
  /// MAP_LOCKED).
  ///   Huge pages (MAP_HUGETLB) are already non-swappable, so this mainly
  ///   affects the anonymous fallback path. May fail due to RLIMIT_MEMLOCK.
  // (1) Convenience constructor — two-level huge page fallback.
  // Aborts if memory allocation fails (startup-time use).
  explicit ObjectPool(std::size_t capacity,
                      PrefaultPolicy pf = PrefaultPolicy::kPopulateWrite,
                      int numa_node = -1, bool lock_pages = false)
      : capacity_(capacity) {

    if (capacity_ >
        std::numeric_limits<std::size_t>::max() / sizeof(NodeType)) {
      mk::sys::log::signal_log("[Critical] ObjectPool capacity overflow!\n");
      std::abort();
    }

    const std::size_t bytes = capacity_ * sizeof(NodeType);
    const RegionIntentConfig cfg{
        .size = bytes, .numa_node = numa_node, .lock_pages = lock_pages};
    memory_block_ = allocate_region(cfg, to_region_intent(pf));
    init_free_list();
  }

  // (2) MmapRegion-accepting constructor — caller decides allocation strategy.
  // The caller creates the MmapRegion using any factory (anonymous, huge pages,
  // shared memory, file-backed) and passes it here. The pool takes ownership.
  //
  // This is the same separation DPDK rte_ring uses: the ring metadata and
  // the backing memory are independent concerns.
  //
  // @param region   Pre-allocated MmapRegion. Must be valid and at least
  //                 capacity * sizeof(NodeType) bytes.
  // @param capacity Number of objects to manage from the region.
  ObjectPool(MmapRegion region, std::size_t capacity) : capacity_(capacity) {
    if (!region.is_valid()) {
      mk::sys::log::signal_log("[Critical] ObjectPool: invalid MmapRegion\n");
      std::abort();
    }
    if (capacity_ >
        std::numeric_limits<std::size_t>::max() / sizeof(NodeType)) {
      mk::sys::log::signal_log("[Critical] ObjectPool capacity overflow!\n");
      std::abort();
    }
    const std::size_t size_in_bytes = capacity_ * sizeof(NodeType);
    if (region.size() < size_in_bytes) {
      mk::sys::log::signal_log(
          "[Critical] ObjectPool: MmapRegion too small (need=", size_in_bytes,
          ", got=", region.size(), ")\n");
      std::abort();
    }

    memory_block_ = std::move(region);
    init_free_list();
  }

  [[nodiscard]] T *allocate() noexcept {
    NodeType *node = free_list_.try_pop();
    if (node == nullptr) [[unlikely]] {
      return nullptr;
    }
    return &node->data;
  }

  void deallocate(T *obj) noexcept {
    if (obj == nullptr) {
      return;
    }
    char *raw_ptr = reinterpret_cast<char *>(obj);
    constexpr std::size_t kOffset = offsetof(NodeType, data);
    auto *node = reinterpret_cast<NodeType *>(raw_ptr - kOffset);
    free_list_.push(node);
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  /// Emptiness check. For LockFreeStack (concurrent), uses a relaxed atomic
  /// load — the result is immediately stale. For SingleThreadStack (single-
  /// threaded), the result is exact. In either case, do NOT use for control
  /// flow in concurrent environments (e.g., `if (!empty()) allocate()`).
  [[nodiscard]] bool empty() const noexcept { return free_list_.empty(); }

private:
  /// Pushes all capacity_ nodes onto the free list.
  /// Called once from constructors after memory_block_ is set.
  void init_free_list() noexcept {
    auto *nodes = static_cast<NodeType *>(memory_block_.get());
    for (std::size_t i = 0; i < capacity_; ++i) {
      free_list_.push(&nodes[i]);
    }
  }

  std::size_t capacity_;
  StackType free_list_;

  // MmapRegion owns the backing memory. Destructor calls munmap()
  // automatically.
  MmapRegion memory_block_;
};

} // namespace mk::sys::memory
