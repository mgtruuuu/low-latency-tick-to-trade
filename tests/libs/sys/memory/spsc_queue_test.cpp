/**
 * @file spsc_queue_test.cpp
 * @brief GTest-based tests for SPSCQueue -- runtime-sized SPSC queue.
 *
 * Google Test reference (see fixed_spsc_queue_test.cpp for basics):
 *
 *   EXPECT_TRUE / EXPECT_FALSE  -- Bool condition check (non-fatal)
 *   ASSERT_TRUE / ASSERT_FALSE  -- Bool condition check (fatal, stops on fail)
 *   EXPECT_EQ(expected, actual)  -- Value equality comparison
 *   EXPECT_NE(a, b)              -- Value inequality comparison
 *   ASSERT_NE(ptr, nullptr)      -- Pointer non-null check (fatal)
 *
 *   ASSERT_TRUE(opt.has_value())
 *     When verifying std::optional: if has_value() fails, accessing the
 *     value is UB. Always use ASSERT (fatal) before accessing the value.
 *
 * Test plan:
 *   1. Basic push/pop
 *   2. FIFO ordering
 *   3. Full / empty boundary
 *   4. Drain
 *   5. External buffer constructor
 *   6. round_up_capacity
 *   7. required_buffer_size
 *   8. create() non-owning factory
 *   9. try_push_batch
 *  10. Multi-thread stress test
 */

#include "sys/memory/spsc_queue.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

// ============================================================================
// Test Fixture
// ============================================================================
//
// SPSCQueue is non-owning: caller provides the buffer. The fixture allocates
// a stack buffer and constructs the queue pointing to it.

class SPSCQueueTest : public ::testing::Test {
protected:
  using Queue = mk::sys::memory::SPSCQueue<std::uint64_t>;

  alignas(64) std::uint64_t buf_[8]{};
  Queue q_{buf_, sizeof(buf_), 8};
};

// ============================================================================
// 1. Basic Push / Pop
// ============================================================================

TEST_F(SPSCQueueTest, PopFromEmptyReturnsFalse) {
  std::uint64_t val = 0;
  EXPECT_FALSE(q_.try_pop(val));
}

TEST_F(SPSCQueueTest, PushOneThenPopReturnsIt) {
  ASSERT_TRUE(q_.try_push(42));

  std::uint64_t val = 0;
  ASSERT_TRUE(q_.try_pop(val));
  EXPECT_EQ(42U, val);

  EXPECT_FALSE(q_.try_pop(val));
}

// ============================================================================
// 2. FIFO Order Verification
// ============================================================================

TEST_F(SPSCQueueTest, FIFOOrder) {
  for (std::uint64_t i = 0; i < 8; ++i) {
    ASSERT_TRUE(q_.try_push(i));
  }
  for (std::uint64_t i = 0; i < 8; ++i) {
    std::uint64_t val = 0;
    ASSERT_TRUE(q_.try_pop(val));
    EXPECT_EQ(i, val);
  }
}

// ============================================================================
// 3. Full / Empty Boundary
// ============================================================================

TEST_F(SPSCQueueTest, PushFailsWhenFull) {
  for (std::uint64_t i = 0; i < 8; ++i) {
    ASSERT_TRUE(q_.try_push(i));
  }
  EXPECT_FALSE(q_.try_push(999));
}

TEST_F(SPSCQueueTest, PushSucceedsAfterPopFromFull) {
  for (std::uint64_t i = 0; i < 8; ++i) {
    ASSERT_TRUE(q_.try_push(i));
  }

  std::uint64_t val = 0;
  ASSERT_TRUE(q_.try_pop(val));
  EXPECT_EQ(0U, val);
  EXPECT_TRUE(q_.try_push(999));

  // Remaining order: 1..7, 999
  const std::uint64_t expected[] = {1, 2, 3, 4, 5, 6, 7, 999};
  for (auto exp : expected) {
    ASSERT_TRUE(q_.try_pop(val));
    EXPECT_EQ(exp, val);
  }
  EXPECT_FALSE(q_.try_pop(val));
}

// ============================================================================
// 4. Drain
// ============================================================================

TEST_F(SPSCQueueTest, DrainReturnsAllItems) {
  for (std::uint64_t i = 0; i < 5; ++i) {
    ASSERT_TRUE(q_.try_push(i + 10));
  }

  std::uint64_t batch[8]{};
  const std::size_t n = q_.drain(batch);
  EXPECT_EQ(5U, n);

  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(i + 10, batch[i]);
  }

  std::uint64_t dummy = 0;
  EXPECT_FALSE(q_.try_pop(dummy));
}

// ============================================================================
// 5. External Buffer Constructor
// ============================================================================

TEST(SPSCQueueNonOwning, ExternalBufferWorks) {
  using Queue = mk::sys::memory::SPSCQueue<std::uint64_t>;
  constexpr std::uint32_t kCap = 8;
  alignas(64) std::uint64_t storage[kCap]{};

  Queue q(storage, sizeof(storage), kCap);

  for (std::uint64_t i = 0; i < kCap; ++i) {
    ASSERT_TRUE(q.try_push(i + 100));
  }
  EXPECT_FALSE(q.try_push(999)); // full

  for (std::uint64_t i = 0; i < kCap; ++i) {
    std::uint64_t val = 0;
    ASSERT_TRUE(q.try_pop(val));
    EXPECT_EQ(i + 100, val);
  }

  std::uint64_t dummy = 0;
  EXPECT_FALSE(q.try_pop(dummy));
}

// ============================================================================
// 6. round_up_capacity
// ============================================================================
//
// Pure static function test: no Fixture needed.

TEST(SPSCQueueCapacity, RoundUpCapacity) {
  using Queue = mk::sys::memory::SPSCQueue<std::uint64_t>;

  // Edge cases
  EXPECT_EQ(0U, Queue::round_up_capacity(0)); // error sentinel
  EXPECT_EQ(2U, Queue::round_up_capacity(1)); // minimum is 2
  EXPECT_EQ(2U, Queue::round_up_capacity(2)); // already valid

  // Non-power-of-two -> next power-of-two
  EXPECT_EQ(4U, Queue::round_up_capacity(3));
  EXPECT_EQ(8U, Queue::round_up_capacity(5));
  EXPECT_EQ(1024U, Queue::round_up_capacity(1000));
  EXPECT_EQ(1024U, Queue::round_up_capacity(1023));

  // Already power-of-two -> unchanged
  EXPECT_EQ(4U, Queue::round_up_capacity(4));
  EXPECT_EQ(1024U, Queue::round_up_capacity(1024));
}

// ============================================================================
// 7. required_buffer_size
// ============================================================================

TEST(SPSCQueueCapacity, RequiredBufferSize) {
  using Queue = mk::sys::memory::SPSCQueue<std::uint64_t>;

  EXPECT_EQ(0U, Queue::required_buffer_size(0));
  EXPECT_EQ(2 * sizeof(std::uint64_t), Queue::required_buffer_size(2));
  EXPECT_EQ(1024 * sizeof(std::uint64_t), Queue::required_buffer_size(1024));
}

// ============================================================================
// 8. create() -- Non-Owning Factory
// ============================================================================

TEST(SPSCQueueFactory, CreateNonOwningValidInput) {
  using Queue = mk::sys::memory::SPSCQueue<std::uint64_t>;
  alignas(64) std::uint64_t storage[8]{};

  auto q = Queue::create(storage, sizeof(storage), 8);
  ASSERT_TRUE(q.has_value());
  EXPECT_EQ(8U, q->capacity()); // NOLINT(bugprone-unchecked-optional-access)

  ASSERT_TRUE(q->try_push(77)); // NOLINT(bugprone-unchecked-optional-access)
  std::uint64_t val = 0;
  ASSERT_TRUE(q->try_pop(val)); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(77U, val);
}

TEST(SPSCQueueFactory, CreateNonOwningRejectsNonPow2) {
  using Queue = mk::sys::memory::SPSCQueue<std::uint64_t>;
  alignas(64) std::uint64_t storage[8]{};

  // capacity 7 (not power-of-two) -> nullopt
  auto bad = Queue::create(storage, sizeof(storage), 7);
  EXPECT_FALSE(bad.has_value());
}

TEST(SPSCQueueFactory, CreateNonOwningRejectsNull) {
  using Queue = mk::sys::memory::SPSCQueue<std::uint64_t>;

  auto bad = Queue::create(nullptr, 0, 8);
  EXPECT_FALSE(bad.has_value());
}

// ============================================================================
// 9. try_push_batch
// ============================================================================

TEST_F(SPSCQueueTest, BatchPushFillsAvailableSlots) {
  // Batch push 5 items into empty queue (capacity 8)
  std::uint64_t items[] = {10, 20, 30, 40, 50};
  EXPECT_EQ(5U, q_.try_push_batch(items, 5));

  // 3 slots remain; pushing 5 more inserts only 3
  std::uint64_t more[] = {60, 70, 80, 90, 100};
  EXPECT_EQ(3U, q_.try_push_batch(more, 5));

  // Queue full -> 0
  std::uint64_t one[] = {999};
  EXPECT_EQ(0U, q_.try_push_batch(one, 1));

  // Verify FIFO order
  const std::uint64_t expected[] = {10, 20, 30, 40, 50, 60, 70, 80};
  for (auto exp : expected) {
    std::uint64_t val = 0;
    ASSERT_TRUE(q_.try_pop(val));
    EXPECT_EQ(exp, val);
  }

  std::uint64_t dummy = 0;
  EXPECT_FALSE(q_.try_pop(dummy));
}

// ============================================================================
// 10. Multi-Thread Stress Test (1P / 1C)
// ============================================================================
//
// Thread safety note for gtest macros:
//   EXPECT: records failure but does not stop the thread.
//   ASSERT: returns from the current function only (not main).
// Safe pattern: collect results via variables in workers,
// then verify with EXPECT/ASSERT on the main thread.

TEST(SPSCQueueStress, SingleProducerSingleConsumer) {
  constexpr std::uint64_t kCount = 2'000'000;
  constexpr std::uint32_t kCap = 1024;
  alignas(64) std::uint64_t stress_buf[kCap]{};
  mk::sys::memory::SPSCQueue<std::uint64_t> q(stress_buf, sizeof(stress_buf),
                                              kCap);

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kCount; ++i) {
      while (!q.try_push(i)) {
        // spin -- queue full
      }
    }
  });

  std::uint64_t consumer_last = 0;
  bool consumer_ok = true;

  std::thread consumer([&] {
    std::uint64_t expected = 0;
    while (expected < kCount) {
      std::uint64_t val = 0;
      if (q.try_pop(val)) {
        if (val != expected) {
          consumer_ok = false;
          consumer_last = expected;
          return;
        }
        ++expected;
      }
    }
    consumer_last = expected;
  });

  producer.join();
  consumer.join();

  EXPECT_TRUE(consumer_ok) << "FIFO violation at index " << consumer_last;
  EXPECT_EQ(kCount, consumer_last);

  std::uint64_t leftover = 0;
  EXPECT_FALSE(q.try_pop(leftover));
}
