/**
 * @file concurrent_object_pool_test.cpp
 * @brief GTest-based tests for ConcurrentObjectPool -- fixed-size, MPMC pool.
 *
 * Additional Google Test concepts:
 *
 *   EXPECT_GE(a, b) / EXPECT_LE(a, b):
 *     Greater-or-Equal / Less-or-Equal comparisons.
 *     Useful for range validation.
 *
 *   constexpr values can be verified at runtime via gtest macros:
 *     constexpr std::size_t kCap = ...;
 *     EXPECT_GE(kCap, 1000u);
 *
 * Test plan:
 *   1. Huge Page alignment verification (calculate_optimal_capacity)
 *   2. Basic allocate / deallocate
 *   3. Two allocations return different addresses
 *   4. LIFO recycling order
 *   5. Pool exhaustion returns nullptr
 *   6. Multi-threaded stress test (no leaks, no corruption)
 *   7. MmapRegion backing — different prefault policies
 *   8. NUMA binding — constructs with/without numa_node
 *   9. Page locking — constructs with lock_pages=true
 *  10. Death test: constructor aborts on capacity overflow
 *  11. calculate_optimal_capacity returns 0 on overflow
 */

#include "sys/memory/concurrent_object_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

// ============================================================================
// Test Data Type
// ============================================================================

struct Order {
  // Atomic id for TSan cleanliness: if the pool has a bug and returns the
  // same object to two threads, non-atomic access would be UB (data race).
  // Using atomic means TSan won't flag the test itself, and we still detect
  // corruption via the value check. relaxed ordering suffices — we only
  // need atomicity, not inter-thread synchronization.
  std::atomic<std::uint64_t> id;
  double price;
  double quantity;
  char side;
  char padding[32];
};

// ============================================================================
// P0 safety-gate documentation (compile-time)
// ============================================================================
//
// ConcurrentObjectPool gates T on is_aggregate_v (construction axis) in
// addition to is_trivially_destructible_v (destruction axis). is_aggregate is a
// conservative C++20 stand-in for implicit-lifetime (the precise trait
// std::is_implicit_lifetime is C++23). These static_asserts pin, at compile
// time, that the gate actually closes the gap:
//
//   - `NotImplicitLifetime` has a user-provided copy constructor, which
//     suppresses the implicit trivial move constructor, leaving no trivial
//     eligible constructor. It is therefore NOT an implicit-lifetime type
//     (verified against clang's __builtin_is_implicit_lifetime, which reports
//     0 for this exact shape) — field-writing its raw storage would be UB.
//     Yet it IS trivially destructible and standard-layout, so it would pass
//     the old gate. The new is_aggregate_v gate rejects it.
//
//   - `Order` (the pooled type) IS an aggregate, so it remains accepted.
//
// This is a negative test expressed via traits rather than a "fails to
// compile" instantiation, because ConcurrentObjectPool's constraint is a hard
// static_assert (not SFINAE), so instantiating
// ConcurrentObjectPool<NotImplicitLifetime> could not be observed from within
// a compiling TU.
namespace {
// The user-provided copy constructor is intentionally NOT `= default`: a
// user-provided copy ctor suppresses the implicit trivial move ctor, leaving
// no trivial eligible constructor, which is exactly what makes this type NOT
// implicit-lifetime while it stays trivially destructible. Defaulting it would
// make it trivial again and destroy the counterexample, so clang-tidy's
// "use = default" / special-member-functions suggestions are deliberately
// suppressed here (justified false positive per the project NOLINT policy).
// NOLINTBEGIN(modernize-use-equals-default,cppcoreguidelines-special-member-functions)
struct NotImplicitLifetime {
  int x{0};
  NotImplicitLifetime(const NotImplicitLifetime &o) : x(o.x) {}
};
// NOLINTEND(modernize-use-equals-default,cppcoreguidelines-special-member-functions)
static_assert(std::is_trivially_destructible_v<NotImplicitLifetime>,
              "counterexample is trivially destructible — passes the old gate");
static_assert(std::is_standard_layout_v<NotImplicitLifetime>,
              "counterexample is standard-layout — passes the NodeType gate");
static_assert(!std::is_aggregate_v<NotImplicitLifetime>,
              "counterexample is NOT an aggregate — the new gate rejects it, "
              "closing the implicit-lifetime hole");
static_assert(std::is_aggregate_v<Order>,
              "Order is an aggregate — the pool accepts it");
} // namespace

// ============================================================================
// Test Fixture
// ============================================================================
//
// Computes 2MB Huge Page-aligned capacity via calculate_optimal_capacity(),
// then creates a pool of that size.

class ConcurrentObjectPoolTest : public ::testing::Test {
protected:
  using PoolType = mk::sys::memory::ConcurrentObjectPool<Order>;

  static constexpr std::size_t kMinRequired = 1000;
  static constexpr std::size_t kCapacity =
      PoolType::calculate_optimal_capacity(kMinRequired);

  PoolType pool_{kCapacity};
};

// ============================================================================
// 1. Huge Page Alignment
// ============================================================================
//
// Verifies that the capacity returned by calculate_optimal_capacity()
// produces a total byte count that fits within a 2MB-aligned chunk.
// The actual memory is backed by MmapRegion (huge pages with THP fallback).

TEST(ConcurrentObjectPoolAlignment, CapacityAlignedToHugePage) {
  using PoolType = mk::sys::memory::ConcurrentObjectPool<Order>;
  constexpr std::size_t kCap = PoolType::calculate_optimal_capacity(1000);

  // Capacity must be at least the minimum requested
  EXPECT_GE(kCap, 1000U);

  // Total bytes must fit within the aligned chunk.
  // Note: node_size may not evenly divide 2MB, so total may be slightly
  // less than the chunk size. This is expected -- MmapRegion handles
  // huge page allocation (MAP_HUGETLB or THP fallback) and alignment.
  using NodeType = mk::sys::memory::LockFreeStack<Order>::NodeType;
  constexpr std::size_t kTotal = kCap * sizeof(NodeType);
  constexpr std::size_t kHugePage = std::size_t{2} * 1024 * 1024;
  constexpr std::size_t kRawNeeded = std::size_t{1000} * sizeof(NodeType);
  constexpr std::size_t kAlignedChunk =
      ((kRawNeeded + kHugePage - 1) / kHugePage) * kHugePage;

  EXPECT_LE(kTotal, kAlignedChunk)
      << "Total " << kTotal << " exceeds aligned chunk " << kAlignedChunk;
  EXPECT_GT(kTotal, kRawNeeded)
      << "Capacity should cover at least the minimum requested objects";
}

// ============================================================================
// 2. Basic Allocate / Deallocate
// ============================================================================

TEST_F(ConcurrentObjectPoolTest, AllocateReturnsNonNull) {
  Order *o = pool_.allocate();
  // ASSERT_NE: if nullptr, subsequent dereference would be UB -- must be fatal
  ASSERT_NE(nullptr, o);
  pool_.deallocate(o);
}

// ============================================================================
// 3. Two Allocations Return Different Addresses
// ============================================================================

TEST_F(ConcurrentObjectPoolTest, TwoAllocationsReturnDifferentAddresses) {
  Order *o1 = pool_.allocate();
  Order *o2 = pool_.allocate();
  ASSERT_NE(nullptr, o1);
  ASSERT_NE(nullptr, o2);
  EXPECT_NE(o1, o2);
  pool_.deallocate(o2);
  pool_.deallocate(o1);
}

// ============================================================================
// 4. LIFO Recycling
// ============================================================================
//
// The internal LockFreeStack is LIFO.
// Deallocate then immediately allocate should return the same address.

TEST_F(ConcurrentObjectPoolTest, DeallocatedObjectIsRecycledLIFO) {
  Order *o1 = pool_.allocate();
  ASSERT_NE(nullptr, o1);
  pool_.deallocate(o1);

  Order *o2 = pool_.allocate();
  EXPECT_EQ(o1, o2);
  pool_.deallocate(o2);
}

// ============================================================================
// 5. Pool Exhaustion
// ============================================================================

TEST_F(ConcurrentObjectPoolTest, AllocateReturnsNullWhenExhausted) {
  std::vector<Order *> allocated;
  allocated.reserve(kCapacity);

  for (std::size_t i = 0; i < kCapacity; ++i) {
    Order *o = pool_.allocate();
    ASSERT_NE(nullptr, o) << "Allocation failed at index " << i;
    allocated.push_back(o);
  }

  // Must return nullptr when exhausted
  EXPECT_EQ(nullptr, pool_.allocate());

  for (Order *o : allocated) {
    pool_.deallocate(o);
  }
}

// ============================================================================
// 6. Multi-Thread Stress Test
// ============================================================================

TEST_F(ConcurrentObjectPoolTest, StressTestNoLeaksOrCorruption) {
  constexpr int kIterations = 50'000;
  unsigned int n_threads = std::thread::hardware_concurrency();
  if (n_threads == 0) {
    n_threads = 4;
  }

  std::atomic<bool> corruption{false};

  auto stress_fn = [&](int thread_id) {
    for (int i = 0; i < kIterations; ++i) {
      Order *o = pool_.allocate();
      if (o) [[likely]] {
        const auto expected = static_cast<std::uint64_t>(i);
        o->id.store(expected, std::memory_order_relaxed);
        o->price = 100.0 + (i % 10);
        o->quantity = 1.0;
        o->side = (thread_id % 2 == 0) ? 'B' : 'S';

        if (o->id.load(std::memory_order_relaxed) != expected) {
          corruption.store(true, std::memory_order_relaxed);
        }

        pool_.deallocate(o);
      } else {
        std::this_thread::yield();
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(n_threads);
  for (unsigned int i = 0; i < n_threads; ++i) {
    threads.emplace_back(stress_fn, static_cast<int>(i));
  }
  for (auto &t : threads) {
    t.join();
  }

  EXPECT_FALSE(corruption.load()) << "Data corruption detected during stress";

  // Leak check: all objects must be returned to the pool
  std::size_t recovered = 0;
  while (pool_.allocate() != nullptr) {
    ++recovered;
  }
  EXPECT_EQ(kCapacity, recovered)
      << "Leaked " << (kCapacity - recovered) << " objects";
}

// ============================================================================
// 7. MmapRegion Backing — Construction with Different Prefault Policies
// ============================================================================
//
// Verifies that the pool constructs successfully with both prefault policies.
// kPopulateWrite is the default (prefaults all pages); kNone defers faults.

TEST(ConcurrentObjectPoolMmapBacking, ConstructsWithPopulateWrite) {
  using PoolType = mk::sys::memory::ConcurrentObjectPool<Order>;
  using PrefaultPolicy = mk::sys::memory::PrefaultPolicy;

  constexpr std::size_t kCap = PoolType::calculate_optimal_capacity(100);
  PoolType pool(kCap, PrefaultPolicy::kPopulateWrite);

  // Pool must be fully functional after construction.
  Order *o = pool.allocate();
  ASSERT_NE(nullptr, o);
  o->id.store(42, std::memory_order_relaxed);
  EXPECT_EQ(42U, o->id.load(std::memory_order_relaxed));
  pool.deallocate(o);
}

TEST(ConcurrentObjectPoolMmapBacking, ConstructsWithNoPrefault) {
  using PoolType = mk::sys::memory::ConcurrentObjectPool<Order>;
  using PrefaultPolicy = mk::sys::memory::PrefaultPolicy;

  constexpr std::size_t kCap = PoolType::calculate_optimal_capacity(100);
  PoolType pool(kCap, PrefaultPolicy::kNone);

  // Pages are lazily faulted on first access, but allocation must still work.
  Order *o = pool.allocate();
  ASSERT_NE(nullptr, o);
  o->id.store(99, std::memory_order_relaxed);
  EXPECT_EQ(99U, o->id.load(std::memory_order_relaxed));
  pool.deallocate(o);
}

TEST(ConcurrentObjectPoolMmapBacking, ConstructsWithManualWrite) {
  using PoolType = mk::sys::memory::ConcurrentObjectPool<Order>;
  using PrefaultPolicy = mk::sys::memory::PrefaultPolicy;

  constexpr std::size_t kCap = PoolType::calculate_optimal_capacity(100);
  PoolType pool(kCap, PrefaultPolicy::kManualWrite);

  Order *o = pool.allocate();
  ASSERT_NE(nullptr, o);
  o->id.store(7, std::memory_order_relaxed);
  EXPECT_EQ(7U, o->id.load(std::memory_order_relaxed));
  pool.deallocate(o);
}

// ============================================================================
// 8. NUMA Binding
// ============================================================================
//
// Verifies that the pool constructs successfully with a NUMA node parameter.
// On systems without NUMA, bind_numa_node() silently fails (best-effort),
// but the pool must still be fully functional.

TEST(ConcurrentObjectPoolNumaBinding, ConstructsWithNumaNode) {
  using PoolType = mk::sys::memory::ConcurrentObjectPool<Order>;
  using PrefaultPolicy = mk::sys::memory::PrefaultPolicy;

  constexpr std::size_t kCap = PoolType::calculate_optimal_capacity(100);

  // numa_node = 0: bind to node 0 (most common on single-socket systems).
  // On non-NUMA systems, bind_numa_node returns false but pool still works.
  PoolType pool(kCap, PrefaultPolicy::kPopulateWrite, /*numa_node=*/0);

  Order *o = pool.allocate();
  ASSERT_NE(nullptr, o);
  o->id.store(42, std::memory_order_relaxed);
  EXPECT_EQ(42U, o->id.load(std::memory_order_relaxed));
  pool.deallocate(o);
}

TEST(ConcurrentObjectPoolNumaBinding, ConstructsWithNoBinding) {
  using PoolType = mk::sys::memory::ConcurrentObjectPool<Order>;
  using PrefaultPolicy = mk::sys::memory::PrefaultPolicy;

  constexpr std::size_t kCap = PoolType::calculate_optimal_capacity(100);

  // numa_node = -1 (default): no NUMA binding, normal prefault path.
  PoolType pool(kCap, PrefaultPolicy::kPopulateWrite, /*numa_node=*/-1);

  Order *o = pool.allocate();
  ASSERT_NE(nullptr, o);
  o->id.store(99, std::memory_order_relaxed);
  EXPECT_EQ(99U, o->id.load(std::memory_order_relaxed));
  pool.deallocate(o);
}

// ============================================================================
// 9. Page Locking (MAP_LOCKED)
// ============================================================================
//
// Verifies that the pool constructs successfully with lock_pages=true.
// May fail on systems with low RLIMIT_MEMLOCK, in which case the pool
// still constructs (MAP_LOCKED failure is non-fatal at the mmap level —
// the kernel may silently ignore it or the factory succeeds without locking).

TEST(ConcurrentObjectPoolPageLocking, ConstructsWithLockedPages) {
  using PoolType = mk::sys::memory::ConcurrentObjectPool<Order>;
  using PrefaultPolicy = mk::sys::memory::PrefaultPolicy;

  constexpr std::size_t kCap = PoolType::calculate_optimal_capacity(100);
  PoolType pool(kCap, PrefaultPolicy::kPopulateWrite, /*numa_node=*/-1,
                /*lock_pages=*/true);

  Order *o = pool.allocate();
  ASSERT_NE(nullptr, o);
  o->id.store(55, std::memory_order_relaxed);
  EXPECT_EQ(55U, o->id.load(std::memory_order_relaxed));
  pool.deallocate(o);
}

// ============================================================================
// 10. MmapRegion-Accepting Constructor
// ============================================================================
//
// Verifies that the pool works when the caller provides a pre-allocated
// MmapRegion. This enables shared memory pools, file-backed pools, etc.

TEST(ConcurrentObjectPoolMmapRegion, ConstructsWithCallerProvidedRegion) {
  using PoolType = mk::sys::memory::ConcurrentObjectPool<Order>;
  using NodeType = PoolType::NodeType;

  constexpr std::size_t kCap = 64;
  const std::size_t size_in_bytes = kCap * sizeof(NodeType);

  // Caller allocates via anonymous mmap (could be shared memory, file, etc.)
  auto region = mk::sys::memory::MmapRegion::allocate_anonymous(
      size_in_bytes, mk::sys::memory::PrefaultPolicy::kPopulateWrite);
  ASSERT_TRUE(region.has_value()) << "Failed to allocate MmapRegion";

  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  PoolType pool(std::move(*region), kCap);
  EXPECT_EQ(kCap, pool.capacity());

  // Verify basic allocate/deallocate works
  Order *o = pool.allocate();
  ASSERT_NE(nullptr, o);
  o->id.store(42, std::memory_order_relaxed);
  EXPECT_EQ(42U, o->id.load(std::memory_order_relaxed));
  pool.deallocate(o);
}

TEST(ConcurrentObjectPoolMmapRegion, ExhaustsAndRecyclesWithExternalRegion) {
  using PoolType = mk::sys::memory::ConcurrentObjectPool<Order>;
  using NodeType = PoolType::NodeType;

  constexpr std::size_t kCap = 64;
  const std::size_t size_in_bytes = kCap * sizeof(NodeType);

  auto region = mk::sys::memory::MmapRegion::allocate_anonymous(
      size_in_bytes, mk::sys::memory::PrefaultPolicy::kPopulateWrite);
  ASSERT_TRUE(region.has_value());

  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  PoolType pool(std::move(*region), kCap);

  // Exhaust all objects
  std::vector<Order *> allocated;
  allocated.reserve(kCap);
  for (std::size_t i = 0; i < kCap; ++i) {
    Order *o = pool.allocate();
    ASSERT_NE(nullptr, o) << "Allocation failed at index " << i;
    allocated.push_back(o);
  }
  EXPECT_EQ(nullptr, pool.allocate()); // exhausted

  // Return all and verify recycling
  for (Order *o : allocated) {
    pool.deallocate(o);
  }
  Order *recycled = pool.allocate();
  ASSERT_NE(nullptr, recycled);
  pool.deallocate(recycled);
}

// ============================================================================
// 11. Death Test: Constructor Aborts on Capacity Overflow
// ============================================================================
//
// The constructor checks that capacity * sizeof(NodeType) doesn't overflow
// std::size_t. If it does, it calls std::abort() (unrecoverable).
// Google Test convention: death test suite names end with "DeathTest".

using ConcurrentObjectPoolDeathTest = ConcurrentObjectPoolTest;

TEST_F(ConcurrentObjectPoolDeathTest, ConstructorAbortsOnCapacityOverflow) {
  using PoolType = mk::sys::memory::ConcurrentObjectPool<Order>;

  // SIZE_MAX objects: capacity * sizeof(NodeType) would overflow.
  EXPECT_DEATH(
      { const PoolType pool(std::numeric_limits<std::size_t>::max()); },
      ".*overflow.*");
}

// ============================================================================
// 11. calculate_optimal_capacity Returns 0 on Overflow
// ============================================================================
//
// In constexpr context, calculate_optimal_capacity cannot abort, so it
// returns 0 to signal that the requested capacity is unreasonable.

TEST(ConcurrentObjectPoolAlignment, CalculateOptimalCapacityOverflowReturnsZero) {
  using PoolType = mk::sys::memory::ConcurrentObjectPool<Order>;

  // Multiplication overflow: SIZE_MAX objects.
  constexpr std::size_t kOverflow = PoolType::calculate_optimal_capacity(
      std::numeric_limits<std::size_t>::max());
  EXPECT_EQ(0U, kOverflow);

  // align_up overflow: request just under SIZE_MAX / node_size so the
  // multiply succeeds but align_up(bytes, 2MB) would wrap around.
  using NodeType = mk::sys::memory::LockFreeStack<Order>::NodeType;
  constexpr std::size_t kJustUnder =
      std::numeric_limits<std::size_t>::max() / sizeof(NodeType);
  constexpr std::size_t kAlignOverflow =
      PoolType::calculate_optimal_capacity(kJustUnder);
  EXPECT_EQ(0U, kAlignOverflow);
}
