/**
 * @file fixed_spsc_queue_test.cpp
 * @brief GTest-based tests for FixedSPSCQueue -- compile-time-sized SPSC queue.
 *
 * Google Test key concepts:
 *
 *   1. TEST(Suite, Name)  -- Standalone test. Starts fresh each time.
 *   2. TEST_F(Fixture, Name) -- Uses a test Fixture. Common init via SetUp().
 *   3. EXPECT_* vs ASSERT_*:
 *      - EXPECT_EQ(a, b): Non-fatal on failure. Test continues.
 *        Useful when checking multiple conditions; see all failures at once.
 *      - ASSERT_EQ(a, b): Fatal on failure. Current test stops immediately.
 *        Use when subsequent code depends on the result (e.g., must pop
 *        successfully before inspecting the value).
 *   4. Linking GTest::gtest_main provides main() automatically.
 *   5. Run specific tests via --gtest_filter="Suite.Name".
 *
 * Test plan:
 *   1. Basic single-thread push/pop
 *   2. FIFO ordering verification
 *   3. Full / empty boundary conditions
 *   4. Drain
 *   5. try_push_batch (partial and full)
 *   6. Multi-thread stress test (1 producer + 1 consumer)
 *   7. Placement new in POSIX shared memory (cross-process IPC proof)
 */

#include "sys/memory/fixed_spsc_queue.hpp"

#include "sys/memory/mmap_region.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

// ============================================================================
// Test Fixture (base class for TEST_F)
// ============================================================================
//
// Inheriting from ::testing::Test allows using TEST_F with this class name.
// A fresh object is created for each TEST_F, so no state is shared between
// tests.
//
// SetUp()   -- Called before each test (prefer over constructor)
// TearDown() -- Called after each test (prefer over destructor)
//
// Here we use a FixedSPSCQueue with capacity=8 for all fixture-based tests.

class FixedSPSCQueueTest : public ::testing::Test {
protected:
  // Type alias: available inside Fixture + TEST_F
  using Queue = mk::sys::memory::FixedSPSCQueue<std::uint64_t, 8>;

  Queue q_; // Default-constructed fresh for each TEST_F
};

// ============================================================================
// 1. Basic Push / Pop
// ============================================================================
//
// TEST_F(FixtureName, TestName) -- can use Fixture members (q_) directly.

TEST_F(FixedSPSCQueueTest, PopFromEmptyReturnsFalse) {
  std::uint64_t val = 0;
  // EXPECT_FALSE: try_pop must return false (queue is empty).
  // Use EXPECT for non-fatal (continue on failure), ASSERT to stop.
  EXPECT_FALSE(q_.try_pop(val));
}

TEST_F(FixedSPSCQueueTest, PushOneThenPopReturnsIt) {
  // ASSERT_TRUE: if push fails, subsequent pop/value checks are meaningless.
  ASSERT_TRUE(q_.try_push(42));

  std::uint64_t val = 0;
  ASSERT_TRUE(q_.try_pop(val));

  // EXPECT_EQ(expected, actual): compares two values.
  // On failure, prints "Expected: 42, Actual: ...".
  EXPECT_EQ(42U, val);

  // Queue should be empty again
  EXPECT_FALSE(q_.try_pop(val));
}

// ============================================================================
// 2. FIFO Order Verification
// ============================================================================

TEST_F(FixedSPSCQueueTest, FIFOOrder) {
  for (std::uint64_t i = 0; i < 8; ++i) {
    ASSERT_TRUE(q_.try_push(i));
  }

  for (std::uint64_t i = 0; i < 8; ++i) {
    std::uint64_t val = 0;
    ASSERT_TRUE(q_.try_pop(val));
    // Must come out in insertion order (FIFO)
    EXPECT_EQ(i, val);
  }
}

// ============================================================================
// 3. Full / Empty Boundary
// ============================================================================

TEST_F(FixedSPSCQueueTest, PushFailsWhenFull) {
  // Fill all 8 slots
  for (std::uint64_t i = 0; i < 8; ++i) {
    ASSERT_TRUE(q_.try_push(i));
  }

  // 9th push must fail
  EXPECT_FALSE(q_.try_push(999));
}

TEST_F(FixedSPSCQueueTest, PushSucceedsAfterPopFromFull) {
  for (std::uint64_t i = 0; i < 8; ++i) {
    ASSERT_TRUE(q_.try_push(i));
  }

  // Pop one, then push should succeed again
  std::uint64_t val = 0;
  ASSERT_TRUE(q_.try_pop(val));
  EXPECT_EQ(0U, val);
  EXPECT_TRUE(q_.try_push(999));

  // Verify remaining order: 1,2,3,4,5,6,7,999
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

TEST_F(FixedSPSCQueueTest, DrainReturnsAllItems) {
  for (std::uint64_t i = 0; i < 5; ++i) {
    ASSERT_TRUE(q_.try_push(i + 10));
  }

  std::uint64_t batch[8]{};
  const std::size_t n = q_.drain(batch);
  EXPECT_EQ(5U, n);

  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(i + 10, batch[i]);
  }

  // Queue must be empty after drain
  std::uint64_t dummy = 0;
  EXPECT_FALSE(q_.try_pop(dummy));
}

// ============================================================================
// 5. try_push_batch
// ============================================================================

TEST_F(FixedSPSCQueueTest, BatchPushFillsAvailableSlots) {
  // Batch push 5 items into empty queue (capacity 8)
  std::uint64_t items[] = {10, 20, 30, 40, 50};
  EXPECT_EQ(5U, q_.try_push_batch(items, 5));

  // Only 3 slots remain; pushing 5 more should insert only 3
  std::uint64_t more[] = {60, 70, 80, 90, 100};
  EXPECT_EQ(3U, q_.try_push_batch(more, 5));

  // Queue is full; push returns 0
  std::uint64_t one[] = {999};
  EXPECT_EQ(0U, q_.try_push_batch(one, 1));

  // Verify FIFO order: 10,20,30,40,50,60,70,80
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
// 6. Multi-Thread Stress Test (1P / 1C)
// ============================================================================
//
// TEST() -- Standalone test without Fixture. Useful when a different capacity
// is needed.
//
// NOTE: gtest EXPECT/ASSERT macros in worker threads record results on
// that thread. If ASSERT fails in a worker, only that thread's function
// returns (not main). The safe pattern is to collect results via flags in
// workers, then do final verification on the main thread.

TEST(FixedSPSCQueueStress, SingleProducerSingleConsumer) {
  mk::sys::memory::FixedSPSCQueue<std::uint64_t, 1024> q;
  constexpr std::uint64_t kCount = 2'000'000;

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kCount; ++i) {
      while (!q.try_push(i)) {
        // spin -- queue full
      }
    }
  });

  // Collect results in worker, verify on main thread
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
          return; // Early exit on error
        }
        ++expected;
      }
    }
    consumer_last = expected;
  });

  producer.join();
  consumer.join();

  // Final verification on main thread
  EXPECT_TRUE(consumer_ok) << "FIFO violation at index " << consumer_last;
  EXPECT_EQ(kCount, consumer_last);

  std::uint64_t leftover = 0;
  EXPECT_FALSE(q.try_pop(leftover));
}

// ============================================================================
// 7. Placement New in POSIX Shared Memory
// ============================================================================
//
// FixedSPSCQueue works in shared memory because:
//   - All state (head_, tail_, buf_) is inline in the struct (no pointers)
//   - std::atomic<uint32_t> is lock-free on x86-64 (no mutex/futex)
//   - MESI cache coherency operates on physical cache lines, not virtual
//     address spaces — two processes mapping the same page see each other's
//     stores the same way two threads do
//
// This test exercises the exact path used in cross-process IPC:
//   shm_open → mmap → placement new → push/pop from separate mapping

TEST(FixedSPSCQueueShmTest, PlacementNewInSharedMemory) {
  using Queue = mk::sys::memory::FixedSPSCQueue<std::uint64_t, 64>;
  static constexpr std::string_view kTestShmName = "/mk_test_fixed_spsc";
  const auto size = sizeof(Queue);

  // Producer side: create shared memory.
  auto producer_region = mk::sys::memory::MmapRegion::open_shared(
      kTestShmName, size, mk::sys::memory::ShmMode::kCreateOrOpen,
      mk::sys::memory::PrefaultPolicy::kPopulateWrite);
  ASSERT_TRUE(producer_region.has_value()) << "Failed to create shared memory";

  // Placement new — construct the queue in shared memory.
  // NOLINTNEXTLINE(*-owning-memory, bugprone-unchecked-optional-access)
  auto *producer_q = new (producer_region->data()) Queue{};

  // Consumer side: open existing shared memory (separate virtual mapping).
  auto consumer_region = mk::sys::memory::MmapRegion::open_shared(
      kTestShmName, size, mk::sys::memory::ShmMode::kOpenExisting,
      mk::sys::memory::PrefaultPolicy::kPopulateRead);
  ASSERT_TRUE(consumer_region.has_value());

  // NOLINTNEXTLINE(bugprone-unchecked-optional-access, *-pro-type-reinterpret-cast)
  auto *consumer_q = reinterpret_cast<Queue *>(consumer_region->data());

  // Producer pushes through one mapping, consumer pops through another.
  ASSERT_TRUE(producer_q->try_push(12345ULL));
  std::uint64_t out = 0;
  ASSERT_TRUE(consumer_q->try_pop(out));
  EXPECT_EQ(out, 12345ULL);

  ::shm_unlink(std::string(kTestShmName).c_str());
}
