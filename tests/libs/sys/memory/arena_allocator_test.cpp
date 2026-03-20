/**
 * @file arena_allocator_test.cpp
 * @brief GTest-based tests for arena_allocator.hpp.
 *
 * Test plan:
 *   1. Arena (non-owning) — basic alloc, alignment, OOM, reset, move, observers
 *   2. Arena (owning, MmapRegion) — construction, alloc, reset, move, observers
 *   3. Arena (owning, convenience) — construction, alloc, capacity
 *   4. ArenaAllocator<T> — STL vector usage, allocator equality
 *   5. Death tests — allocator abort on OOM
 */

#include "sys/memory/arena_allocator.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

using mk::sys::memory::Arena;
using mk::sys::memory::ArenaAllocator;
using mk::sys::memory::MmapRegion;

// ============================================================================
// 1. Arena (non-owning) — external buffer via std::span
// ============================================================================

TEST(ArenaTest, BasicAllocation) {
  alignas(64) std::byte buf[4096];
  Arena a(std::span<std::byte>{buf, sizeof(buf)});

  void *p = a.alloc(100, alignof(void *));
  ASSERT_NE(nullptr, p);
  EXPECT_GE(a.used(), 100U);
  EXPECT_FALSE(a.owns_memory());
}

TEST(ArenaTest, MultipleAllocations) {
  alignas(64) std::byte buf[4096];
  Arena a(std::span<std::byte>{buf, sizeof(buf)});

  void *p1 = a.alloc(64, 8);
  void *p2 = a.alloc(64, 8);
  ASSERT_NE(nullptr, p1);
  ASSERT_NE(nullptr, p2);
  EXPECT_NE(p1, p2);
}

TEST(ArenaTest, AlignmentMinEnforcement) {
  alignas(64) std::byte buf[4096];
  Arena a(std::span<std::byte>{buf, sizeof(buf)});

  // Request alignment 1, should be promoted to alignof(void*).
  void *p = a.alloc(1, 1);
  ASSERT_NE(nullptr, p);
  auto addr = reinterpret_cast<std::uintptr_t>(p);
  EXPECT_EQ(0U, addr % alignof(void *));
}

TEST(ArenaTest, CustomAlignment) {
  alignas(64) std::byte buf[4096];
  Arena a(std::span<std::byte>{buf, sizeof(buf)});

  void *p = a.alloc(10, 64);
  ASSERT_NE(nullptr, p);
  auto addr = reinterpret_cast<std::uintptr_t>(p);
  EXPECT_EQ(0U, addr % 64);
}

TEST(ArenaTest, OomReturnsNullptr) {
  alignas(8) std::byte buf[64];
  Arena a(std::span<std::byte>{buf, sizeof(buf)});

  // First allocation takes most of the buffer.
  void *p = a.alloc(60, 8);
  ASSERT_NE(nullptr, p);

  // Second allocation exceeds remaining space.
  EXPECT_EQ(nullptr, a.alloc(16, 8));
}

TEST(ArenaTest, Reset) {
  alignas(64) std::byte buf[256];
  Arena a(std::span<std::byte>{buf, sizeof(buf)});

  (void)a.alloc(200, 8);
  EXPECT_GE(a.used(), 200U);

  a.reset();
  EXPECT_EQ(0U, a.used());
  EXPECT_EQ(256U, a.remaining());

  // Can allocate again after reset.
  void *p = a.alloc(200, 8);
  ASSERT_NE(nullptr, p);
}

TEST(ArenaTest, Observers) {
  alignas(64) std::byte buf[1024];
  Arena a(std::span<std::byte>{buf, sizeof(buf)});

  EXPECT_EQ(1024U, a.capacity());
  EXPECT_EQ(0U, a.used());
  EXPECT_EQ(1024U, a.remaining());

  (void)a.alloc(100, 8);
  EXPECT_EQ(1024U, a.capacity());
  EXPECT_GE(a.used(), 100U);
  EXPECT_EQ(a.capacity() - a.used(), a.remaining());
}

TEST(ArenaTest, MoveConstructorNonOwning) {
  alignas(64) std::byte buf[1024];
  Arena a(std::span<std::byte>{buf, sizeof(buf)});
  (void)a.alloc(100, 8);

  const Arena b(std::move(a));
  EXPECT_EQ(1024U, b.capacity());
  EXPECT_GE(b.used(), 100U);
  EXPECT_FALSE(b.owns_memory());

  // Source is in moved-from state.
  EXPECT_EQ(0U, a.capacity());  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(0U, a.used());
}

TEST(ArenaTest, MoveAssignmentNonOwning) {
  alignas(64) std::byte buf1[512];
  alignas(64) std::byte buf2[1024];
  Arena a(std::span<std::byte>{buf1, sizeof(buf1)});
  Arena b(std::span<std::byte>{buf2, sizeof(buf2)});

  (void)a.alloc(100, 8);
  b = std::move(a);

  EXPECT_EQ(512U, b.capacity());
  EXPECT_GE(b.used(), 100U);
  EXPECT_EQ(0U, a.capacity());  // NOLINT(bugprone-use-after-move)
}

// ============================================================================
// 2. Arena (owning) — MmapRegion constructor
// ============================================================================

TEST(ArenaOwningTest, MmapRegionConstructor) {
  auto region = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(region.has_value());
  const auto region_size = region->size();  // NOLINT(bugprone-unchecked-optional-access)

  const Arena a(std::move(*region));       // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_TRUE(a.owns_memory());
  // MmapRegion may round up to page boundary, so capacity >= requested.
  EXPECT_GE(a.capacity(), 4096U);
  EXPECT_EQ(region_size, a.capacity());
  EXPECT_EQ(0U, a.used());
}

TEST(ArenaOwningTest, MmapRegionAllocAndReset) {
  auto region = MmapRegion::allocate_anonymous(8192);
  ASSERT_TRUE(region.has_value());

  Arena a(std::move(*region));  // NOLINT(bugprone-unchecked-optional-access)

  void *p1 = a.alloc(1024, 8);
  ASSERT_NE(nullptr, p1);
  EXPECT_GE(a.used(), 1024U);

  void *p2 = a.alloc(1024, 64);
  ASSERT_NE(nullptr, p2);
  auto addr = reinterpret_cast<std::uintptr_t>(p2);
  EXPECT_EQ(0U, addr % 64);

  a.reset();
  EXPECT_EQ(0U, a.used());
  EXPECT_EQ(a.capacity(), a.remaining());

  // Can allocate again after reset.
  void *p3 = a.alloc(1024, 8);
  ASSERT_NE(nullptr, p3);
}

TEST(ArenaOwningTest, MoveConstructorOwning) {
  auto region = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(region.has_value());

  Arena a(std::move(*region));  // NOLINT(bugprone-unchecked-optional-access)
  (void)a.alloc(100, 8);
  const auto cap = a.capacity();

  const Arena b(std::move(a));
  EXPECT_TRUE(b.owns_memory());
  EXPECT_EQ(cap, b.capacity());
  EXPECT_GE(b.used(), 100U);

  // Source is in moved-from state.
  EXPECT_EQ(0U, a.capacity());  // NOLINT(bugprone-use-after-move)
  EXPECT_FALSE(a.owns_memory());
}

TEST(ArenaOwningTest, MoveAssignmentOwning) {
  auto region1 = MmapRegion::allocate_anonymous(4096);
  auto region2 = MmapRegion::allocate_anonymous(8192);
  ASSERT_TRUE(region1.has_value());
  ASSERT_TRUE(region2.has_value());

  Arena a(std::move(*region1));  // NOLINT(bugprone-unchecked-optional-access)
  Arena b(std::move(*region2));  // NOLINT(bugprone-unchecked-optional-access)
  const auto cap_a = a.capacity();

  (void)a.alloc(100, 8);
  b = std::move(a);

  EXPECT_EQ(cap_a, b.capacity());
  EXPECT_GE(b.used(), 100U);
  EXPECT_TRUE(b.owns_memory());
  EXPECT_EQ(0U, a.capacity());  // NOLINT(bugprone-use-after-move)
}

TEST(ArenaOwningTest, MoveFromOwningToNonOwning) {
  // Move an owning arena into a non-owning one. The old MmapRegion should be
  // properly cleaned up via the swap-based move assignment.
  alignas(64) std::byte buf[512];
  Arena non_owning(std::span<std::byte>{buf, sizeof(buf)});

  auto region = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(region.has_value());
  Arena owning(std::move(*region));  // NOLINT(bugprone-unchecked-optional-access)
  const auto cap = owning.capacity();

  (void)owning.alloc(100, 8);
  non_owning = std::move(owning);

  EXPECT_EQ(cap, non_owning.capacity());
  EXPECT_TRUE(non_owning.owns_memory());
  EXPECT_EQ(0U, owning.capacity());  // NOLINT(bugprone-use-after-move)
}

// ============================================================================
// 3. Arena (owning) — convenience constructor (two-level huge page fallback)
// ============================================================================

TEST(ArenaOwningTest, ConvenienceConstructor) {
  const Arena a(65536);
  EXPECT_TRUE(a.owns_memory());
  // MmapRegion rounds up to page (or huge page) boundary.
  EXPECT_GE(a.capacity(), 65536U);
  EXPECT_EQ(0U, a.used());
}

TEST(ArenaOwningTest, ConvenienceConstructorAlloc) {
  Arena a(65536);

  void *p = a.alloc(1024, 8);
  ASSERT_NE(nullptr, p);
  EXPECT_GE(a.used(), 1024U);

  // Verify alignment.
  void *p2 = a.alloc(256, 64);
  ASSERT_NE(nullptr, p2);
  auto addr = reinterpret_cast<std::uintptr_t>(p2);
  EXPECT_EQ(0U, addr % 64);
}

TEST(ArenaOwningTest, ConvenienceConstructorReset) {
  Arena a(65536);

  (void)a.alloc(32768, 8);
  EXPECT_GE(a.used(), 32768U);

  a.reset();
  EXPECT_EQ(0U, a.used());

  // Can allocate again.
  void *p = a.alloc(32768, 8);
  ASSERT_NE(nullptr, p);
}

TEST(ArenaOwningTest, ConvenienceConstructorObservers) {
  Arena a(65536);

  EXPECT_GE(a.capacity(), 65536U);
  EXPECT_EQ(0U, a.used());
  EXPECT_EQ(a.capacity(), a.remaining());
  EXPECT_TRUE(a.owns_memory());

  (void)a.alloc(100, 8);
  EXPECT_GE(a.used(), 100U);
  EXPECT_EQ(a.capacity() - a.used(), a.remaining());
}

// ============================================================================
// 4. ArenaAllocator<T> — STL vector usage
// ============================================================================

TEST(ArenaAllocatorTest, VectorWithNonOwningArena) {
  alignas(64) std::byte buf[8192];
  Arena a(std::span<std::byte>{buf, sizeof(buf)});
  ArenaAllocator<int> const alloc(a);

  std::vector<int, ArenaAllocator<int>> v(alloc);
  v.reserve(100);
  for (int i = 0; i < 100; ++i) {
    v.push_back(i);
  }

  EXPECT_EQ(100U, v.size());
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(i, v[i]);
  }
}

TEST(ArenaAllocatorTest, VectorWithOwningArena) {
  Arena a(65536);
  ArenaAllocator<int> const alloc(a);

  std::vector<int, ArenaAllocator<int>> v(alloc);
  v.reserve(1000);
  for (int i = 0; i < 1000; ++i) {
    v.push_back(i);
  }

  EXPECT_EQ(1000U, v.size());
  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(i, v[i]);
  }
}

TEST(ArenaAllocatorTest, AllocatorEquality) {
  alignas(64) std::byte buf1[4096];
  alignas(64) std::byte buf2[4096];
  Arena a1(std::span<std::byte>{buf1, sizeof(buf1)});
  Arena a2(std::span<std::byte>{buf2, sizeof(buf2)});

  ArenaAllocator<int> const alloc_a1(a1);
  ArenaAllocator<int> const alloc_a1b(a1);       // same arena
  ArenaAllocator<int> const alloc_a2(a2);        // different arena
  ArenaAllocator<double> const alloc_a1_dbl(a1); // different T, same arena

  EXPECT_TRUE(alloc_a1 == alloc_a1b);
  EXPECT_FALSE(alloc_a1 == alloc_a2);
  // Cross-type comparison: same arena → equal.
  EXPECT_TRUE(alloc_a1 == alloc_a1_dbl);
}

// ============================================================================
// 5. Death tests
// ============================================================================

using ArenaVecInt = std::vector<int, ArenaAllocator<int>>;

TEST(ArenaAllocatorDeathTest, AllocatorAbortsOnArenaOom) {
  alignas(8) std::byte buf[64];
  Arena a(std::span<std::byte>{buf, sizeof(buf)});
  ArenaAllocator<int> const alloc(a);

  // reserve(1000) requests 4000 bytes from a 64-byte arena → abort.
  EXPECT_DEATH(
      {
        ArenaVecInt v(alloc);
        v.reserve(1000);
      },
      ".*");
}
