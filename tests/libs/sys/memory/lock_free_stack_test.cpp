/**
 * @file lock_free_stack_test.cpp
 * @brief GTest-based tests for LockFreeStack (Treiber Stack).
 *
 * Multi-threaded testing pattern in Google Test:
 *
 *   When using EXPECT / ASSERT macros in worker threads:
 *   - EXPECT: records failure in gtest but does NOT stop the thread.
 *   - ASSERT: only returns from the current function (NOT main).
 *   Therefore, the safe pattern is to collect results via bool/atomic flags
 *   in workers, then do final verification with EXPECT/ASSERT on the main
 *   thread.
 *
 * Test plan:
 *   1. Pop from empty stack returns nullptr
 *   2. LIFO order verification
 *   3. Data persistence after push/pop
 *   4. All nodes recoverable
 *   5. Multi-threaded stress test (40 threads, 12 nodes)
 */

#include "sys/memory/lock_free_stack.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

// ============================================================================
// Test Data Type
// ============================================================================

struct Order {
  std::uint64_t id = 0;
  double price = 0.0;
};

// ============================================================================
// Test Fixture
// ============================================================================
//
// Static pool of 12 nodes + a LockFreeStack to simulate Object Pool scenario.
// SetUp() pushes all nodes onto the stack to initialize the pool.
// Each TEST_F gets a fresh Fixture object, ensuring test isolation.

class LockFreeStackTest : public ::testing::Test {
protected:
  using Stack = mk::sys::memory::LockFreeStack<Order>;
  using Node = Stack::NodeType;

  // [Test Tip] Small pool size forces high contention (rapid node recycling)
  static constexpr std::size_t kPoolSize = 12;

  // 64-byte aligned to sit on cache-line boundary
  alignas(64) Node node_pool_[kPoolSize];
  Stack stack_;

  void SetUp() override {
    for (auto &i : node_pool_) {
      stack_.push(&i);
    }
  }
};

// ============================================================================
// 1. Pop from Empty Stack
// ============================================================================
//
// Uses TEST() (no Fixture) since we need a completely empty stack.

TEST(LockFreeStackEmpty, PopFromEmptyReturnsNullptr) {
  mk::sys::memory::LockFreeStack<Order> empty_stack;
  EXPECT_EQ(nullptr, empty_stack.try_pop());
}

// ============================================================================
// 2. LIFO Order Verification
// ============================================================================
//
// SetUp() pushed node_pool_[0], [1], ..., [11] in order.
// Due to LIFO, [11] (last pushed) must be popped first.

TEST_F(LockFreeStackTest, PopReturnsLastPushedNode) {
  Node *top = stack_.try_pop();
  ASSERT_NE(nullptr, top);
  EXPECT_EQ(&node_pool_[kPoolSize - 1], top);
  stack_.push(top);
}

// ============================================================================
// 3. Data Persistence
// ============================================================================
//
// Verifies that node data survives a push/pop cycle.
// ASSERT first (precondition), then EXPECT for value comparisons.

TEST_F(LockFreeStackTest, DataPersistsAfterPushPop) {
  Node *n1 = stack_.try_pop();
  ASSERT_NE(nullptr, n1);

  n1->data.id = 999;
  n1->data.price = 42.5;

  stack_.push(n1);

  // Pop again -- LIFO returns the node we just pushed
  Node *n2 = stack_.try_pop();
  ASSERT_EQ(n1, n2);
  EXPECT_EQ(999U, n2->data.id);
  EXPECT_DOUBLE_EQ(42.5, n2->data.price);

  stack_.push(n2);
}

// ============================================================================
// 4. All Nodes Recoverable
// ============================================================================

TEST_F(LockFreeStackTest, AllNodesAreRecoverable) {
  std::size_t count = 0;
  while (stack_.try_pop() != nullptr) {
    ++count;
  }
  EXPECT_EQ(kPoolSize, count);
  EXPECT_EQ(nullptr, stack_.try_pop());
}

// ============================================================================
// 5. Multi-Thread Stress Test (40 Threads x 12 Nodes)
// ============================================================================
//
// 40 threads compete for 12 nodes via alloc/free loops.
// Validates under extreme contention:
//   - No ABA problem
//   - No data corruption (two threads owning the same node)
//   - No node leaks (all nodes returned to pool)

TEST_F(LockFreeStackTest, StressTestNoConcurrencyBugs) {
  constexpr int kThreadCount = 40;
  constexpr int kIterations = 100'000;

  std::atomic<std::uint64_t> total_allocs{0};
  std::atomic<bool> corruption_detected{false};

  auto stress_fn = [&](int thread_id) {
    for (int i = 0; i < kIterations; ++i) {
      Node *node = stack_.try_pop();

      if (node) {
        // Exclusive ownership check: write then read back
        node->data.id = static_cast<std::uint64_t>(thread_id);
        node->data.price = 100.0 + i;

        if (std::cmp_not_equal(node->data.id, thread_id)) {
          corruption_detected.store(true, std::memory_order_relaxed);
          return;
        }

        stack_.push(node);
        total_allocs.fetch_add(1, std::memory_order_relaxed);
      } else {
        // Pool empty -- yield CPU so others can free nodes
        std::this_thread::yield();
        --i; // Retry this iteration
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back(stress_fn, i);
  }
  for (auto &t : threads) {
    t.join();
  }

  // Final verification on main thread
  EXPECT_FALSE(corruption_detected.load())
      << "Data corruption detected during stress test";

  // Leak check: all nodes must be back in the pool
  std::size_t recovered = 0;
  while (stack_.try_pop() != nullptr) {
    ++recovered;
  }
  EXPECT_EQ(kPoolSize, recovered)
      << "Node leak: expected " << kPoolSize << ", recovered " << recovered;
}
