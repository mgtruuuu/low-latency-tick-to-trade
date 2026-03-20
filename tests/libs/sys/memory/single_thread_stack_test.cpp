/**
 * @file single_thread_stack_test.cpp
 * @brief GTest-based tests for SingleThreadStack — zero-sync LIFO stack.
 *
 * Mirrors lock_free_stack_test.cpp structure but without multi-threaded tests
 * (SingleThreadStack is explicitly not thread-safe).
 *
 * Test plan:
 *   1. PopFromEmptyReturnsNull
 *   2. LIFOOrder
 *   3. DataPersistence
 *   4. DrainAllNodes
 *   5. PushAfterDrain
 *   6. EmptyReflectsState
 *   7. LargeBatchCycle
 */

#include "sys/memory/single_thread_stack.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

struct Order {
  std::uint64_t id;
  double price;
};

using Stack = mk::sys::memory::SingleThreadStack<Order>;
using Node = Stack::NodeType;

// =============================================================================
// Fixture: pre-allocates a pool of nodes and pushes them onto the stack.
// =============================================================================

class SingleThreadStackTest : public ::testing::Test {
protected:
  static constexpr std::size_t kPoolSize = 16;
  Node node_pool_[kPoolSize]{};
  Stack stack_;

  void SetUp() override {
    for (std::size_t i = 0; i < kPoolSize; ++i) {
      node_pool_[i].data.id = i;
      node_pool_[i].data.price = static_cast<double>(i) * 1.5;
      stack_.push(&node_pool_[i]);
    }
  }
};

// =============================================================================
// 1. PopFromEmptyReturnsNull
// =============================================================================

TEST(SingleThreadStackEmpty, PopFromEmptyReturnsNull) {
  Stack stack;
  EXPECT_EQ(nullptr, stack.try_pop());
  EXPECT_TRUE(stack.empty());
}

// =============================================================================
// 2. LIFOOrder
// =============================================================================

TEST_F(SingleThreadStackTest, LIFOOrder) {
  // Last pushed was node_pool_[kPoolSize-1], so it should be popped first.
  for (std::size_t i = kPoolSize; i > 0; --i) {
    Node *node = stack_.try_pop();
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(&node_pool_[i - 1], node);
  }
  EXPECT_EQ(nullptr, stack_.try_pop());
}

// =============================================================================
// 3. DataPersistence
// =============================================================================

TEST_F(SingleThreadStackTest, DataPersistence) {
  // Pop the top node and verify its data is intact.
  Node *node = stack_.try_pop();
  ASSERT_NE(nullptr, node);
  EXPECT_EQ(kPoolSize - 1, node->data.id);
  EXPECT_DOUBLE_EQ(static_cast<double>(kPoolSize - 1) * 1.5, node->data.price);
}

// =============================================================================
// 4. DrainAllNodes
// =============================================================================

TEST_F(SingleThreadStackTest, DrainAllNodes) {
  std::size_t count = 0;
  while (stack_.try_pop() != nullptr) {
    ++count;
  }
  EXPECT_EQ(kPoolSize, count);
  EXPECT_TRUE(stack_.empty());
}

// =============================================================================
// 5. PushAfterDrain
// =============================================================================

TEST_F(SingleThreadStackTest, PushAfterDrain) {
  // Drain.
  while (stack_.try_pop() != nullptr) {
  }
  EXPECT_TRUE(stack_.empty());

  // Push one back and verify.
  stack_.push(&node_pool_[0]);
  EXPECT_FALSE(stack_.empty());
  Node *node = stack_.try_pop();
  ASSERT_NE(nullptr, node);
  EXPECT_EQ(&node_pool_[0], node);
  EXPECT_TRUE(stack_.empty());
}

// =============================================================================
// 6. EmptyReflectsState
// =============================================================================

TEST(SingleThreadStackEmpty, EmptyReflectsState) {
  Stack stack;
  EXPECT_TRUE(stack.empty());

  Node node{};
  stack.push(&node);
  EXPECT_FALSE(stack.empty());

  stack.try_pop();
  EXPECT_TRUE(stack.empty());
}

// =============================================================================
// 7. LargeBatchCycle — allocate all, return all, allocate all again
// =============================================================================

TEST_F(SingleThreadStackTest, LargeBatchCycle) {
  // Pop all.
  Node *popped[kPoolSize]{};
  for (auto & i : popped) {
    i = stack_.try_pop();
    ASSERT_NE(nullptr, i);
  }
  EXPECT_TRUE(stack_.empty());

  // Push all back.
  for (auto & i : popped) {
    stack_.push(i);
  }
  EXPECT_FALSE(stack_.empty());

  // Pop all again — should get exactly kPoolSize nodes.
  std::size_t count = 0;
  while (stack_.try_pop() != nullptr) {
    ++count;
  }
  EXPECT_EQ(kPoolSize, count);
}

} // namespace
