/**
 * @file single_thread_object_pool_test.cpp
 * @brief GTest-based tests for SingleThreadObjectPool — index-based,
 *        single-threaded pool (raw T[] slots + ds::IndexFreeStack).
 *
 * Test plan:
 *   1. Basic allocate / deallocate
 *   2. Two allocations return different addresses
 *   3. Pool exhaustion returns nullptr
 *   4. Deallocated slot is reused (LIFO)
 *   5. All slots recoverable (exhaust -> free all -> re-exhaust)
 *   6. Pointer-to-index round-trip (dealloc by pointer, realloc, no aliasing)
 *   7. empty() reflects state
 *   8. Custom over-aligned T — slot addresses respect alignof(T)
 *   9. Caller-provided MmapRegion constructor
 *  10. required_region_size() static helper sanity
 *  11. Death: constructor aborts on capacity overflow
 *  12. Death (debug only): double-free aborts via IndexFreeStack tracker
 */

#include "sys/memory/single_thread_object_pool.hpp"

#include "sys/memory/mmap_utils.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

namespace mk::sys::memory {
namespace {

// Pooled type — an aggregate, trivially destructible (passes the P0 gate).
struct Item {
  std::uint64_t id;
  double price;
  char pad[16];
};
static_assert(std::is_aggregate_v<Item> &&
                  std::is_trivially_destructible_v<Item>,
              "Item must satisfy the SingleThreadObjectPool T gate");

// Over-aligned aggregate.
struct alignas(64) AlignedItem {
  std::uint64_t data[8]{};
};
static_assert(std::is_aggregate_v<AlignedItem> &&
              std::is_trivially_destructible_v<AlignedItem>);

// =============================================================================
// Fixture
// =============================================================================

class SingleThreadObjectPoolTest : public ::testing::Test {
protected:
  static constexpr std::size_t kCapacity = 1000;
  using PoolType = SingleThreadObjectPool<Item>;
};

// =============================================================================
// 1. Basic allocate
// =============================================================================

TEST_F(SingleThreadObjectPoolTest, AllocateReturnsNonNull) {
  PoolType pool(kCapacity);
  Item *p = pool.allocate();
  ASSERT_NE(nullptr, p);
  // Field-write the raw storage (the whole point — no placement-new).
  p->id = 42;
  p->price = 3.14;
  EXPECT_EQ(42U, p->id);
  pool.deallocate(p);
}

// =============================================================================
// 2. Distinct addresses
// =============================================================================

TEST_F(SingleThreadObjectPoolTest, TwoAllocationsReturnDifferentAddresses) {
  PoolType pool(kCapacity);
  Item *a = pool.allocate();
  Item *b = pool.allocate();
  ASSERT_NE(nullptr, a);
  ASSERT_NE(nullptr, b);
  EXPECT_NE(a, b);
  pool.deallocate(a);
  pool.deallocate(b);
}

// =============================================================================
// 3. Exhaustion
// =============================================================================

TEST_F(SingleThreadObjectPoolTest, ExhaustionReturnsNullptr) {
  PoolType pool(kCapacity);
  std::vector<Item *> held;
  held.reserve(kCapacity);
  for (std::size_t i = 0; i < kCapacity; ++i) {
    Item *p = pool.allocate();
    ASSERT_NE(nullptr, p) << "allocation " << i << " unexpectedly failed";
    held.push_back(p);
  }
  EXPECT_TRUE(pool.empty());
  EXPECT_EQ(nullptr, pool.allocate()); // exhausted
  for (Item *p : held) {
    pool.deallocate(p);
  }
}

// =============================================================================
// 4. LIFO reuse
// =============================================================================

TEST_F(SingleThreadObjectPoolTest, DeallocatedSlotIsReusedLIFO) {
  PoolType pool(kCapacity);
  Item *a = pool.allocate();
  Item *b = pool.allocate();
  ASSERT_NE(a, b);

  pool.deallocate(b); // b pushed last
  Item *c = pool.allocate();
  EXPECT_EQ(b, c)
      << "LIFO: the most recently freed slot should come back first";

  pool.deallocate(a);
  pool.deallocate(c);
}

// =============================================================================
// 5. All slots recoverable
// =============================================================================

TEST_F(SingleThreadObjectPoolTest, AllSlotsRecoverable) {
  PoolType pool(kCapacity);
  std::vector<Item *> held;
  held.reserve(kCapacity);
  for (std::size_t i = 0; i < kCapacity; ++i) {
    held.push_back(pool.allocate());
  }
  for (Item *p : held) {
    pool.deallocate(p);
  }
  EXPECT_FALSE(pool.empty());

  // Re-exhaust: must hand out exactly kCapacity again.
  std::size_t reacquired = 0;
  while (pool.allocate() != nullptr) {
    ++reacquired;
  }
  EXPECT_EQ(kCapacity, reacquired);
}

// =============================================================================
// 6. Pointer-to-index round-trip (no aliasing across dealloc/realloc)
// =============================================================================

TEST_F(SingleThreadObjectPoolTest, PointerToIndexRoundTrip) {
  PoolType pool(kCapacity);
  // Allocate all, stamp each with a unique id matching its address ordering.
  std::vector<Item *> held;
  held.reserve(kCapacity);
  for (std::size_t i = 0; i < kCapacity; ++i) {
    Item *p = pool.allocate();
    ASSERT_NE(nullptr, p);
    p->id = static_cast<std::uint64_t>(i);
    held.push_back(p);
  }
  // Deallocate every other one by pointer, then realloc the same count.
  std::size_t freed = 0;
  for (std::size_t i = 0; i < kCapacity; i += 2) {
    pool.deallocate(held[i]);
    ++freed;
  }
  std::size_t regot = 0;
  while (Item *p = pool.allocate()) {
    // Each returned slot must be one of the originals (within the slot array),
    // never an out-of-bounds or aliased address.
    ASSERT_GE(p, held.front());
    ASSERT_LE(p, held.back());
    ++regot;
  }
  EXPECT_EQ(freed, regot);
}

// =============================================================================
// 7. empty() reflects state
// =============================================================================

TEST_F(SingleThreadObjectPoolTest, EmptyReflectsState) {
  PoolType pool(/*capacity=*/2);
  EXPECT_FALSE(pool.empty());
  Item *a = pool.allocate();
  Item *b = pool.allocate();
  EXPECT_TRUE(pool.empty());
  pool.deallocate(a);
  EXPECT_FALSE(pool.empty());
  pool.deallocate(b);
}

// =============================================================================
// 8. Over-aligned T
// =============================================================================

TEST(SingleThreadObjectPoolAlignment, RespectsCustomAlignment) {
  SingleThreadObjectPool<AlignedItem> pool(64);
  std::vector<AlignedItem *> held;
  for (int i = 0; i < 64; ++i) {
    AlignedItem *p = pool.allocate();
    ASSERT_NE(nullptr, p);
    EXPECT_EQ(0U, reinterpret_cast<std::uintptr_t>(p) % alignof(AlignedItem))
        << "slot " << i << " is not 64-byte aligned";
    held.push_back(p);
  }
  for (AlignedItem *p : held) {
    pool.deallocate(p);
  }
}

// =============================================================================
// 9. Caller-provided MmapRegion
// =============================================================================

TEST(SingleThreadObjectPoolRegion, ConstructsFromCallerRegion) {
  constexpr std::size_t kCap = 256;
  const std::size_t bytes =
      SingleThreadObjectPool<Item>::required_region_size(kCap);
  const RegionIntentConfig cfg{
      .size = bytes, .numa_node = -1, .lock_pages = false};
  auto region = try_allocate_region(cfg, RegionIntent::kHotRw);
  ASSERT_TRUE(region.has_value());

  // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE
  SingleThreadObjectPool<Item> pool(std::move(*region), kCap);
  std::size_t n = 0;
  while (pool.allocate() != nullptr) {
    ++n;
  }
  EXPECT_EQ(kCap, n);
}

// =============================================================================
// 10. required_region_size sanity
// =============================================================================

TEST(SingleThreadObjectPoolStatic, RequiredRegionSizeAccountsForBothSections) {
  constexpr std::size_t kCap = 100;
  const std::size_t total =
      SingleThreadObjectPool<Item>::required_region_size(kCap);
  // Must cover the T slot array plus the index buffer.
  EXPECT_GE(total, (kCap * sizeof(Item)) +
                       mk::ds::IndexFreeStack::required_buffer_size(kCap));
}

// =============================================================================
// 11. Death: capacity overflow
// =============================================================================

TEST(SingleThreadObjectPoolDeathTest, ConstructorAbortsOnCapacityOverflow) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_DEATH(
      {
        const SingleThreadObjectPool<Item> pool(
            std::numeric_limits<std::size_t>::max());
        (void)pool;
      },
      "");
}

// capacity == 0: IndexFreeStack cannot represent it (requires >= 1).
TEST(SingleThreadObjectPoolDeathTest, ConstructorAbortsOnZeroCapacity) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_DEATH(
      {
        const SingleThreadObjectPool<Item> pool(0);
        (void)pool;
      },
      "");
}

// capacity > UINT32_MAX: the index free list indexes slots with a uint32_t,
// so a capacity that cannot fit in uint32_t is rejected before allocation.
// (For normal object sizes this UINT32_MAX bound effectively subsumes the
// align_up / partition-sum overflow guards: capacity <= UINT32_MAX keeps
// capacity*sizeof(T) far below SIZE_MAX, so no constructible T is large enough
// to trip those later guards in practice — they remain as cheap, order-safe
// defense-in-depth.)
TEST(SingleThreadObjectPoolDeathTest, ConstructorAbortsWhenCapacityExceedsU32) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_DEATH(
      {
        const SingleThreadObjectPool<Item> pool(
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) +
            1);
        (void)pool;
      },
      "");
}

// =============================================================================
// 12. Death (debug only): double-free
// =============================================================================
//
// In Debug builds IndexFreeStack carries an in_use_ tracker and push() aborts
// on double-free. In NDEBUG that tracker is compiled out, so the guard (and
// this test) only exist in Debug.
#ifndef NDEBUG
TEST(SingleThreadObjectPoolDeathTest, DoubleFreeAborts) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_DEATH(
      {
        SingleThreadObjectPool<Item> pool(8);
        Item *p = pool.allocate();
        pool.deallocate(p);
        pool.deallocate(p); // double free -> IndexFreeStack aborts
      },
      "");
}
#endif

} // namespace
} // namespace mk::sys::memory
