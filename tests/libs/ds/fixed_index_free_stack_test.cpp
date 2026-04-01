/**
 * @file fixed_index_free_stack_test.cpp
 * @brief Tests for mk::ds::FixedIndexFreeStack.
 */

#include "ds/fixed_index_free_stack.hpp"

#include <cstdint>
#include <limits>
#include <set>
#include <vector>

#include <gtest/gtest.h>

namespace {

using mk::ds::FixedIndexFreeStack;

// =============================================================================
// 1. Construction
// =============================================================================

TEST(FixedIndexFreeStackTest, StartsFullWithAllIndices) {
  FixedIndexFreeStack<8> const stack;
  EXPECT_EQ(stack.available(), 8U);
  EXPECT_EQ(stack.capacity(), 8U);
  EXPECT_TRUE(stack.full());
  EXPECT_FALSE(stack.empty());
}

// =============================================================================
// 2. Pop returns unique indices in [0, Capacity)
// =============================================================================

TEST(FixedIndexFreeStackTest, PopReturnsUniqueIndices) {
  constexpr std::size_t kN = 16;
  FixedIndexFreeStack<kN> stack;

  std::set<std::uint32_t> seen;
  for (std::size_t i = 0; i < kN; ++i) {
    std::uint32_t idx = 0;
    ASSERT_TRUE(stack.pop(idx));
    EXPECT_LT(idx, kN);
    EXPECT_TRUE(seen.insert(idx).second) << "Duplicate index: " << idx;
  }

  EXPECT_TRUE(stack.empty());
  EXPECT_EQ(stack.available(), 0U);
}

// =============================================================================
// 3. Pop from empty returns false
// =============================================================================

TEST(FixedIndexFreeStackTest, PopFromEmptyReturnsFalse) {
  FixedIndexFreeStack<2> stack;
  std::uint32_t dummy = 0;
  ASSERT_TRUE(stack.pop(dummy));
  ASSERT_TRUE(stack.pop(dummy));
  EXPECT_FALSE(stack.pop(dummy));
  EXPECT_TRUE(stack.empty());
}

// =============================================================================
// 4. Push recycles indices
// =============================================================================

TEST(FixedIndexFreeStackTest, PushRecyclesIndex) {
  FixedIndexFreeStack<4> stack;

  std::uint32_t idx = 0;
  ASSERT_TRUE(stack.pop(idx));
  EXPECT_EQ(stack.available(), 3U);

  stack.push(idx);
  EXPECT_EQ(stack.available(), 4U);
  EXPECT_TRUE(stack.full());
}

// =============================================================================
// 5. LIFO ordering
// =============================================================================

TEST(FixedIndexFreeStackTest, LIFOOrdering) {
  FixedIndexFreeStack<8> stack;

  // Pop 3 indices.
  std::uint32_t a = 0;
  std::uint32_t b = 0;
  std::uint32_t c = 0;
  ASSERT_TRUE(stack.pop(a));
  ASSERT_TRUE(stack.pop(b));
  ASSERT_TRUE(stack.pop(c));

  // Push back in order: a, b, c.
  stack.push(a);
  stack.push(b);
  stack.push(c);

  // LIFO: should get c, b, a back.
  std::uint32_t out = 0;
  ASSERT_TRUE(stack.pop(out));
  EXPECT_EQ(out, c);
  ASSERT_TRUE(stack.pop(out));
  EXPECT_EQ(out, b);
  ASSERT_TRUE(stack.pop(out));
  EXPECT_EQ(out, a);
}

// =============================================================================
// 6. Full exhaust and refill
// =============================================================================

TEST(FixedIndexFreeStackTest, ExhaustAndRefill) {
  constexpr std::size_t kN = 32;
  FixedIndexFreeStack<kN> stack;

  std::vector<std::uint32_t> indices;
  for (std::size_t i = 0; i < kN; ++i) {
    std::uint32_t idx = 0;
    ASSERT_TRUE(stack.pop(idx));
    indices.push_back(idx);
  }
  EXPECT_TRUE(stack.empty());

  // Return all.
  for (auto idx : indices) {
    stack.push(idx);
  }
  EXPECT_TRUE(stack.full());
  EXPECT_EQ(stack.available(), kN);
}

// =============================================================================
// 7. Capacity of 1
// =============================================================================

TEST(FixedIndexFreeStackTest, CapacityOne) {
  FixedIndexFreeStack<1> stack;
  EXPECT_EQ(stack.available(), 1U);

  std::uint32_t idx = 0;
  ASSERT_TRUE(stack.pop(idx));
  EXPECT_EQ(idx, 0U);
  EXPECT_TRUE(stack.empty());

  std::uint32_t dummy = 0;
  EXPECT_FALSE(stack.pop(dummy));

  stack.push(0);
  EXPECT_TRUE(stack.full());
}

// =============================================================================
// 8. Constexpr construction and query
// =============================================================================

TEST(FixedIndexFreeStackTest, ConstexprConstruction) {
  constexpr FixedIndexFreeStack<4> kStack{};
  static_assert(mk::ds::FixedIndexFreeStack<4>::capacity() == 4);
  static_assert(kStack.available() == 4);
  static_assert(kStack.full());
  static_assert(!kStack.empty());

  // Verify pop() remains constexpr after the out-param API change.
  constexpr auto kPopWorks = [] {
    FixedIndexFreeStack<4> s;
    std::uint32_t out = 0;
    return s.pop(out) && out == 0;
  }();
  static_assert(kPopWorks);
}

// =============================================================================
// 9. Death tests — push() precondition violations abort
// =============================================================================

TEST(FixedIndexFreeStackDeathTest, PushOnFullStackAborts) {
  EXPECT_DEATH(
      {
        FixedIndexFreeStack<2> stack;
        // Stack is full (both indices available). Push without pop.
        stack.push(0);
      },
      "");
}

TEST(FixedIndexFreeStackDeathTest, PushOutOfRangeIndexAborts) {
  EXPECT_DEATH(
      {
        FixedIndexFreeStack<4> stack;
        std::uint32_t dummy = 0;
        (void)stack.pop(dummy); // Make room.
        stack.push(4);          // idx == Capacity → out of range.
      },
      "");
}

TEST(FixedIndexFreeStackDeathTest, PushLargeOutOfRangeIndexAborts) {
  EXPECT_DEATH(
      {
        FixedIndexFreeStack<4> stack;
        std::uint32_t dummy = 0;
        (void)stack.pop(dummy);
        stack.push(std::numeric_limits<std::uint32_t>::max());
      },
      "");
}

// Debug-only death tests for the in_use_ allocation tracker.
// These catch double-free and push-without-pop bugs that would silently
// create duplicate indices in the stack.
#ifndef NDEBUG
TEST(FixedIndexFreeStackDeathTest, DoubleFreeAborts) {
  EXPECT_DEATH(
      {
        FixedIndexFreeStack<4> stack;
        std::uint32_t idx = 0;
        (void)stack.pop(idx);
        stack.push(idx);
        stack.push(idx); // Already returned — double-free.
      },
      "");
}

TEST(FixedIndexFreeStackDeathTest, PushNeverPoppedIndexAborts) {
  EXPECT_DEATH(
      {
        FixedIndexFreeStack<4> stack;
        std::uint32_t popped = 0;
        (void)stack.pop(popped); // Make room.
        // Push a different valid index that was never popped.
        const std::uint32_t other = (popped + 1) % 4;
        stack.push(other);
      },
      "");
}
#endif // NDEBUG

} // namespace
