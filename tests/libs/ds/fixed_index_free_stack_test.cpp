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
    auto idx = stack.pop();
    ASSERT_TRUE(idx.has_value());
    EXPECT_LT(*idx, kN);                                          // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_TRUE(seen.insert(*idx).second) << "Duplicate index: " << *idx;  // NOLINT(bugprone-unchecked-optional-access)
  }

  EXPECT_TRUE(stack.empty());
  EXPECT_EQ(stack.available(), 0U);
}

// =============================================================================
// 3. Pop from empty returns nullopt
// =============================================================================

TEST(FixedIndexFreeStackTest, PopFromEmptyReturnsNullopt) {
  FixedIndexFreeStack<2> stack;
  (void)stack.pop();
  (void)stack.pop();
  EXPECT_EQ(stack.pop(), std::nullopt);
  EXPECT_TRUE(stack.empty());
}

// =============================================================================
// 4. Push recycles indices
// =============================================================================

TEST(FixedIndexFreeStackTest, PushRecyclesIndex) {
  FixedIndexFreeStack<4> stack;

  auto idx = stack.pop();
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(stack.available(), 3U);

  stack.push(*idx);  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(stack.available(), 4U);
  EXPECT_TRUE(stack.full());
}

// =============================================================================
// 5. LIFO ordering
// =============================================================================

TEST(FixedIndexFreeStackTest, LIFOOrdering) {
  FixedIndexFreeStack<8> stack;

  // Pop 3 indices.
  auto a = stack.pop();
  auto b = stack.pop();
  auto c = stack.pop();

  // Push back in order: a, b, c.
  stack.push(*a);  // NOLINT(bugprone-unchecked-optional-access)
  stack.push(*b);  // NOLINT(bugprone-unchecked-optional-access)
  stack.push(*c);  // NOLINT(bugprone-unchecked-optional-access)

  // LIFO: should get c, b, a back.
  EXPECT_EQ(stack.pop(), c);
  EXPECT_EQ(stack.pop(), b);
  EXPECT_EQ(stack.pop(), a);
}

// =============================================================================
// 6. Full exhaust and refill
// =============================================================================

TEST(FixedIndexFreeStackTest, ExhaustAndRefill) {
  constexpr std::size_t kN = 32;
  FixedIndexFreeStack<kN> stack;

  std::vector<std::uint32_t> indices;
  for (std::size_t i = 0; i < kN; ++i) {
    auto idx = stack.pop();
    ASSERT_TRUE(idx.has_value());
    indices.push_back(*idx);  // NOLINT(bugprone-unchecked-optional-access)
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

  auto idx = stack.pop();
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 0U);  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_TRUE(stack.empty());

  EXPECT_EQ(stack.pop(), std::nullopt);

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
        (void)stack.pop(); // Make room.
        stack.push(4);     // idx == Capacity → out of range.
      },
      "");
}

TEST(FixedIndexFreeStackDeathTest, PushLargeOutOfRangeIndexAborts) {
  EXPECT_DEATH(
      {
        FixedIndexFreeStack<4> stack;
        (void)stack.pop();
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
        auto idx = stack.pop();
        stack.push(*idx);  // NOLINT(bugprone-unchecked-optional-access)
        stack.push(*idx); // Already returned — double-free.  // NOLINT(bugprone-unchecked-optional-access)
      },
      "");
}

TEST(FixedIndexFreeStackDeathTest, PushNeverPoppedIndexAborts) {
  EXPECT_DEATH(
      {
        FixedIndexFreeStack<4> stack;
        auto popped = stack.pop(); // Make room.
        // Push a different valid index that was never popped.
        const std::uint32_t other = (*popped + 1) % 4;  // NOLINT(bugprone-unchecked-optional-access)
        stack.push(other);
      },
      "");
}
#endif // NDEBUG

} // namespace
