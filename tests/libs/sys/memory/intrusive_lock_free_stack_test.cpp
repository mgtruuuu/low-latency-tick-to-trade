/**
 * @file intrusive_lock_free_stack_test.cpp
 * @brief GTest-based tests for IntrusiveLockFreeStack (intrusive Treiber
 * Stack).
 *
 * Mirrors the test structure of lock_free_stack_test.cpp but uses the intrusive
 * variant: the test type inherits LockFreeStackHook directly, eliminating the
 * wrapper Node.
 *
 * Test plan:
 *   1. Pop from empty stack returns nullptr
 *   2. LIFO order verification
 *   3. Data persistence after push/pop
 *   4. All nodes recoverable (drain + count)
 *   5. Push after drain succeeds
 *   6. Empty check reflects state
 *   7. Multi-threaded corruption detection (40 threads, 12 nodes)
 *   8. Stress test — no leaks or ABA (12 threads, 50k iterations, 1000 nodes)
 */

#include "sys/memory/intrusive_lock_free_stack.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

// ============================================================================
// Test Data Type — inherits LockFreeStackHook (intrusive)
// ============================================================================
//
// Unlike the non-intrusive test (which uses a plain struct wrapped in Node),
// this type IS the node. The `next` pointer lives inside Order itself via
// inheritance from LockFreeStackHook.

struct Order : mk::sys::memory::LockFreeStackHook {
  std::uint64_t id{0};
  double price{0.0};
};

// ============================================================================
// Test Fixture
// ============================================================================

class IntrusiveLockFreeStackTest : public ::testing::Test {
protected:
  using Stack = mk::sys::memory::IntrusiveLockFreeStack<Order>;

  static constexpr std::size_t kPoolSize = 12;

  // 64-byte aligned for cache-line boundary.
  alignas(64) Order node_pool_[kPoolSize];
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

TEST(IntrusiveLockFreeStackEmpty, PopFromEmptyReturnsNullptr) {
  mk::sys::memory::IntrusiveLockFreeStack<Order> empty_stack;
  EXPECT_EQ(nullptr, empty_stack.try_pop());
  EXPECT_TRUE(empty_stack.empty());
}

// ============================================================================
// 2. LIFO Order Verification
// ============================================================================

TEST_F(IntrusiveLockFreeStackTest, PopReturnsLastPushedNode) {
  Order *top = stack_.try_pop();
  ASSERT_NE(nullptr, top);
  // SetUp pushed [0]..[11] in order; LIFO means [11] comes out first.
  EXPECT_EQ(&node_pool_[kPoolSize - 1], top);
  stack_.push(top);
}

// ============================================================================
// 3. Data Persistence
// ============================================================================
//
// Verifies that data written to the intrusive node survives push/pop.
// Key difference from non-intrusive: no `node->data` indirection — fields
// are accessed directly on the Order* (the object IS the node).

TEST_F(IntrusiveLockFreeStackTest, DataPersistsAfterPushPop) {
  Order *o1 = stack_.try_pop();
  ASSERT_NE(nullptr, o1);

  o1->id = 999;
  o1->price = 42.5;

  stack_.push(o1);

  Order *o2 = stack_.try_pop();
  ASSERT_EQ(o1, o2);
  EXPECT_EQ(999U, o2->id);
  EXPECT_DOUBLE_EQ(42.5, o2->price);

  stack_.push(o2);
}

// ============================================================================
// 4. All Nodes Recoverable
// ============================================================================

TEST_F(IntrusiveLockFreeStackTest, AllNodesAreRecoverable) {
  std::size_t count = 0;
  while (stack_.try_pop() != nullptr) {
    ++count;
  }
  EXPECT_EQ(kPoolSize, count);
  EXPECT_EQ(nullptr, stack_.try_pop());
}

// ============================================================================
// 5. Push After Drain
// ============================================================================

TEST_F(IntrusiveLockFreeStackTest, PushAfterDrainSucceeds) {
  // Drain all nodes.
  Order *popped[kPoolSize];
  for (auto &i : popped) {
    i = stack_.try_pop();
    ASSERT_NE(nullptr, i);
  }
  EXPECT_TRUE(stack_.empty());

  // Push one back, pop it.
  stack_.push(popped[0]);
  EXPECT_FALSE(stack_.empty());

  Order *recovered = stack_.try_pop();
  EXPECT_EQ(popped[0], recovered);
  EXPECT_TRUE(stack_.empty());
}

// ============================================================================
// 6. Empty Check
// ============================================================================

TEST_F(IntrusiveLockFreeStackTest, EmptyReflectsState) {
  EXPECT_FALSE(stack_.empty());

  // Drain all.
  for (std::size_t i = 0; i < kPoolSize; ++i) {
    stack_.try_pop();
  }
  EXPECT_TRUE(stack_.empty());
}

// ============================================================================
// 7. Multi-Threaded Corruption Detection (40 threads, 12 nodes)
// ============================================================================
//
// Same pattern as lock_free_stack_test.cpp: 40 threads compete for 12 nodes.
// Each thread writes its thread_id into the Order, then reads it back.
// Any mismatch means two threads owned the same node simultaneously.
//
// The key difference: no `node->data.id` indirection. We write directly
// to `order->id` because the object IS the node.

TEST_F(IntrusiveLockFreeStackTest, ConcurrentPushPopNoCorruption) {
  constexpr int kThreadCount = 40;
  constexpr int kIterations = 100'000;

  std::atomic<bool> corruption_detected{false};

  auto worker = [&](int thread_id) {
    for (int i = 0; i < kIterations; ++i) {
      Order *order = stack_.try_pop();

      if (order) {
        // Exclusive ownership check.
        order->id = static_cast<std::uint64_t>(thread_id);
        order->price = 100.0 + i;

        if (std::cmp_not_equal(order->id, thread_id)) {
          corruption_detected.store(true, std::memory_order_relaxed);
          stack_.push(order);
          return;
        }

        stack_.push(order);
      } else {
        std::this_thread::yield();
        --i; // Retry this iteration.
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back(worker, i);
  }
  for (auto &t : threads) {
    t.join();
  }

  EXPECT_FALSE(corruption_detected.load())
      << "Data corruption: two threads owned the same node simultaneously";

  // Leak check.
  std::size_t recovered = 0;
  while (stack_.try_pop() != nullptr) {
    ++recovered;
  }
  EXPECT_EQ(kPoolSize, recovered)
      << "Node leak: expected " << kPoolSize << ", recovered " << recovered;
}

// ============================================================================
// 8. Stress Test — Large Pool (12 threads, 50k iterations, 1000 nodes)
// ============================================================================
//
// Tests at larger scale with more nodes and fewer threads to reduce CAS
// contention probability. Validates that the intrusive stack handles sustained
// throughput without leaks or corruption.

TEST_F(IntrusiveLockFreeStackTest, StressTestNoLeaksOrCorruption) {
  // Use a separate, larger pool for this test.
  static constexpr std::size_t kLargePoolSize = 1'000;
  static constexpr int kThreadCount = 12;
  static constexpr int kIterations = 50'000;

  alignas(64) Order large_pool[kLargePoolSize];
  mk::sys::memory::IntrusiveLockFreeStack<Order> large_stack;

  for (auto &i : large_pool) {
    large_stack.push(&i);
  }

  std::atomic<std::uint64_t> total_allocs{0};
  std::atomic<bool> corruption_detected{false};

  auto worker = [&](int thread_id) {
    for (int i = 0; i < kIterations; ++i) {
      Order *order = large_stack.try_pop();

      if (order) {
        order->id = static_cast<std::uint64_t>(thread_id);
        order->price = static_cast<double>(i);

        if (std::cmp_not_equal(order->id, thread_id)) {
          corruption_detected.store(true, std::memory_order_relaxed);
          large_stack.push(order);
          return;
        }

        large_stack.push(order);
        total_allocs.fetch_add(1, std::memory_order_relaxed);
      } else {
        std::this_thread::yield();
        --i;
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back(worker, i);
  }
  for (auto &t : threads) {
    t.join();
  }

  EXPECT_FALSE(corruption_detected.load())
      << "Data corruption detected in stress test";

  // Leak check.
  std::size_t recovered = 0;
  while (large_stack.try_pop() != nullptr) {
    ++recovered;
  }
  EXPECT_EQ(kLargePoolSize, recovered)
      << "Node leak: expected " << kLargePoolSize << ", recovered "
      << recovered;
}
