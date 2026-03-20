/**
 * @file fixed_ring_buffer_test.cpp
 * @brief Tests for mk::ds::FixedRingBuffer — fixed-capacity ring buffer.
 */

#include "ds/fixed_ring_buffer.hpp"

#include <numeric>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using mk::ds::FixedRingBuffer;

// =============================================================================
// 1. EmptyOnConstruction
// =============================================================================

TEST(FixedRingBufferTest, EmptyOnConstruction) {
  FixedRingBuffer<int, 8> const rb;
  EXPECT_EQ(rb.size(), 0U);
  EXPECT_TRUE(rb.empty());
  EXPECT_FALSE(rb.full());
  EXPECT_EQ(rb.capacity(), 8U);
}

// =============================================================================
// 2. PushAndAccess
// =============================================================================

TEST(FixedRingBufferTest, PushAndAccess) {
  FixedRingBuffer<int, 8> rb;
  rb.push(42);

  EXPECT_EQ(rb.size(), 1U);
  EXPECT_FALSE(rb.empty());
  EXPECT_FALSE(rb.full());
  EXPECT_EQ(rb.front(), 42);
  EXPECT_EQ(rb.back(), 42);
  EXPECT_EQ(rb[0], 42);
}

// =============================================================================
// 3. FIFOOrder
// =============================================================================

TEST(FixedRingBufferTest, FIFOOrder) {
  FixedRingBuffer<int, 8> rb;
  for (int i = 0; i < 5; ++i) {
    rb.push(i * 10);
  }

  EXPECT_EQ(rb.size(), 5U);
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(rb[i], static_cast<int>(i) * 10);
  }
  EXPECT_EQ(rb.front(), 0);
  EXPECT_EQ(rb.back(), 40);
}

// =============================================================================
// 4. OverwriteOldest
// =============================================================================

TEST(FixedRingBufferTest, OverwriteOldest) {
  FixedRingBuffer<int, 4> rb;

  // Fill to capacity: [0, 1, 2, 3]
  for (int i = 0; i < 4; ++i) {
    rb.push(i);
  }
  ASSERT_TRUE(rb.full());
  ASSERT_EQ(rb.front(), 0);

  // Push one more — overwrites oldest (0). Buffer: [1, 2, 3, 4]
  rb.push(4);
  EXPECT_TRUE(rb.full());
  EXPECT_EQ(rb.size(), 4U);
  EXPECT_EQ(rb.front(), 1);
  EXPECT_EQ(rb.back(), 4);
  EXPECT_EQ(rb[0], 1);
  EXPECT_EQ(rb[1], 2);
  EXPECT_EQ(rb[2], 3);
  EXPECT_EQ(rb[3], 4);
}

// =============================================================================
// 5. OverwriteMultipleWraps
// =============================================================================

TEST(FixedRingBufferTest, OverwriteMultipleWraps) {
  constexpr std::size_t kCap = 8;
  FixedRingBuffer<int, kCap> rb;

  // Push 3 * capacity elements. Only the last 'capacity' should survive.
  constexpr int kTotal = static_cast<int>(3 * kCap);
  for (int i = 0; i < kTotal; ++i) {
    rb.push(i);
  }

  EXPECT_TRUE(rb.full());
  EXPECT_EQ(rb.size(), kCap);

  // Last kCap elements: [16, 17, 18, 19, 20, 21, 22, 23]
  for (std::size_t i = 0; i < kCap; ++i) {
    EXPECT_EQ(rb[i], kTotal - static_cast<int>(kCap) + static_cast<int>(i));
  }
}

// =============================================================================
// 6. PopFront
// =============================================================================

TEST(FixedRingBufferTest, PopFront) {
  FixedRingBuffer<int, 8> rb;
  for (int i = 0; i < 5; ++i) {
    rb.push(i * 10); // [0, 10, 20, 30, 40]
  }

  rb.pop_front(); // [10, 20, 30, 40]
  EXPECT_EQ(rb.size(), 4U);
  EXPECT_EQ(rb.front(), 10);
  EXPECT_EQ(rb[0], 10);
}

// =============================================================================
// 7. PopFrontToEmpty
// =============================================================================

TEST(FixedRingBufferTest, PopFrontToEmpty) {
  FixedRingBuffer<int, 4> rb;
  rb.push(1);
  rb.push(2);
  rb.push(3);

  rb.pop_front();
  rb.pop_front();
  rb.pop_front();

  EXPECT_TRUE(rb.empty());
  EXPECT_EQ(rb.size(), 0U);
}

// =============================================================================
// 8. ClearResetsEverything
// =============================================================================

TEST(FixedRingBufferTest, ClearResetsEverything) {
  FixedRingBuffer<int, 8> rb;
  for (int i = 0; i < 5; ++i) {
    rb.push(i);
  }
  ASSERT_EQ(rb.size(), 5U);

  rb.clear();
  EXPECT_TRUE(rb.empty());
  EXPECT_EQ(rb.size(), 0U);
  EXPECT_FALSE(rb.full());

  // Push again after clear — should work correctly.
  rb.push(100);
  EXPECT_EQ(rb.size(), 1U);
  EXPECT_EQ(rb.front(), 100);
  EXPECT_EQ(rb.back(), 100);
}

// =============================================================================
// 9. RangeForIteration
// =============================================================================

TEST(FixedRingBufferTest, RangeForIteration) {
  FixedRingBuffer<int, 8> rb;
  for (int i = 0; i < 5; ++i) {
    rb.push(i * 10);
  }

  // Range-for should yield elements in logical order (oldest to newest).
  std::vector<int> result;
  for (const auto &val : rb) {
    result.push_back(val);
  }

  ASSERT_EQ(result.size(), 5U);
  for (std::size_t i = 0; i < result.size(); ++i) {
    EXPECT_EQ(result[i], rb[i]);
  }
}

// =============================================================================
// 10. IteratorRandomAccess
// =============================================================================

TEST(FixedRingBufferTest, IteratorRandomAccess) {
  FixedRingBuffer<int, 8> rb;
  for (int i = 0; i < 5; ++i) {
    rb.push(i);
  }

  auto it_begin = rb.begin();
  auto it_end = rb.end();

  // Distance
  EXPECT_EQ(it_end - it_begin, 5);

  // Advance
  EXPECT_EQ(it_begin + 5, it_end);
  EXPECT_EQ(it_end - 5, it_begin);

  // Subscript
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(it_begin[i], i);
  }

  // Comparison
  EXPECT_TRUE(it_begin < it_end);
  EXPECT_TRUE(it_begin <= it_end);
  EXPECT_TRUE(it_end > it_begin);
  EXPECT_TRUE(it_end >= it_begin);
  EXPECT_TRUE(it_begin == it_begin);
  EXPECT_TRUE(it_begin != it_end);

  // Pre/post increment
  auto it = rb.begin();
  EXPECT_EQ(*it, 0);
  ++it;
  EXPECT_EQ(*it, 1);
  auto prev = it++;
  EXPECT_EQ(*prev, 1);
  EXPECT_EQ(*it, 2);

  // STL algorithm compatibility: std::accumulate
  const int sum = std::accumulate(rb.begin(), rb.end(), 0);
  EXPECT_EQ(sum, 0 + 1 + 2 + 3 + 4);
}

// =============================================================================
// 11. ConstAccess
// =============================================================================

TEST(FixedRingBufferTest, ConstAccess) {
  FixedRingBuffer<int, 8> rb;
  for (int i = 0; i < 5; ++i) {
    rb.push(i * 10);
  }

  const auto &crb = rb;

  EXPECT_EQ(crb.size(), 5U);
  EXPECT_EQ(crb.front(), 0);
  EXPECT_EQ(crb.back(), 40);
  EXPECT_EQ(crb[2], 20);

  // Const iteration
  std::vector<int> result;
  for (const auto &val : crb) {
    result.push_back(val);
  }
  ASSERT_EQ(result.size(), 5U);
  for (std::size_t i = 0; i < result.size(); ++i) {
    EXPECT_EQ(result[i], crb[i]);
  }

  // Iterator → ConstIterator implicit conversion
  const FixedRingBuffer<int, 8>::ConstIterator cit = rb.begin();
  EXPECT_EQ(*cit, 0);
}

// =============================================================================
// 12. PushRvalueOverload (POD type — std::string is forbidden by static_assert)
// =============================================================================

// Realistic HFT type: trivially destructible, default-constructible, POD.
struct Tick {
  int price{0};
  int quantity{0};

  bool operator==(const Tick &other) const noexcept = default;
};

TEST(FixedRingBufferTest, PushRvalueOverload) {
  FixedRingBuffer<Tick, 4> rb;

  const Tick t{.price = 100, .quantity = 50};
  rb.push(t);

  EXPECT_EQ(rb.size(), 1U);
  EXPECT_EQ(rb.front(), (Tick{.price = 100, .quantity = 50}));

  // Push via lvalue (copy overload).
  const Tick t2{.price = 200, .quantity = 75};
  rb.push(t2);
  EXPECT_EQ(rb.back(), (Tick{.price = 200, .quantity = 75}));

  // Overwrite semantics with POD type.
  // Already have 2 elements; push 4 more into capacity-4 buffer.
  // Last 4 survive: {300,0}, {301,1}, {302,2}, {303,3}.
  for (int i = 0; i < 4; ++i) {
    rb.push(Tick{.price = 300 + i, .quantity = i});
  }
  EXPECT_TRUE(rb.full());
  EXPECT_EQ(rb.front(), (Tick{.price = 300, .quantity = 0}));
  EXPECT_EQ(rb.back(), (Tick{.price = 303, .quantity = 3}));
}

// =============================================================================
// 13. OverwriteWrapAround
// =============================================================================

TEST(FixedRingBufferTest, OverwriteWrapAround) {
  // Capacity 4, push 6 elements.
  // Physical layout wraps around the array boundary.
  FixedRingBuffer<int, 4> rb;

  // Push 6: [0, 1, 2, 3, 4, 5]
  // After 4: buf = [0,1,2,3], write_idx=4, count=4
  // Push 4:  buf = [4,1,2,3], write_idx=5, count=4, logical=[1,2,3,4]
  // Push 5:  buf = [4,5,2,3], write_idx=6, count=4, logical=[2,3,4,5]
  for (int i = 0; i < 6; ++i) {
    rb.push(i);
  }

  EXPECT_TRUE(rb.full());
  EXPECT_EQ(rb.size(), 4U);

  // Logical order: [2, 3, 4, 5]
  EXPECT_EQ(rb[0], 2);
  EXPECT_EQ(rb[1], 3);
  EXPECT_EQ(rb[2], 4);
  EXPECT_EQ(rb[3], 5);
  EXPECT_EQ(rb.front(), 2);
  EXPECT_EQ(rb.back(), 5);
}

// =============================================================================
// 14. LargeBufferStress
// =============================================================================

TEST(FixedRingBufferTest, LargeBufferStress) {
  constexpr std::size_t kCap = 1024;
  constexpr int kTotal = 10000;
  FixedRingBuffer<int, kCap> rb;

  for (int i = 0; i < kTotal; ++i) {
    rb.push(i);
  }

  EXPECT_TRUE(rb.full());
  EXPECT_EQ(rb.size(), kCap);

  // The last kCap elements should be [kTotal - kCap, ..., kTotal - 1].
  for (std::size_t i = 0; i < kCap; ++i) {
    EXPECT_EQ(rb[i], kTotal - static_cast<int>(kCap) + static_cast<int>(i));
  }

  // Verify via iterator as well.
  int expected = kTotal - static_cast<int>(kCap);
  for (const auto &val : rb) {
    EXPECT_EQ(val, expected);
    ++expected;
  }
}

// =============================================================================
// 15. Death tests — precondition violations abort (Debug builds only)
// =============================================================================
// Type alias avoids commas inside EXPECT_DEATH macro arguments.
// The preprocessor would otherwise split FixedRingBuffer<int, 4> at the comma.
using RB4 = FixedRingBuffer<int, 4>;

TEST(FixedRingBufferDeathTest, FrontOnEmpty) {
  EXPECT_DEATH(
      {
        RB4 rb;
        (void)rb.front();
      },
      "");
}

TEST(FixedRingBufferDeathTest, BackOnEmpty) {
  EXPECT_DEATH(
      {
        RB4 rb;
        (void)rb.back();
      },
      "");
}

TEST(FixedRingBufferDeathTest, PopFrontOnEmpty) {
  EXPECT_DEATH(
      {
        RB4 rb;
        rb.pop_front();
      },
      "");
}

TEST(FixedRingBufferDeathTest, SubscriptOutOfBounds) {
  EXPECT_DEATH(
      {
        RB4 rb;
        rb.push(42);
        (void)rb[1]; // size() == 1, index 1 is out of bounds
      },
      "");
}

TEST(FixedRingBufferDeathTest, ConstFrontOnEmpty) {
  EXPECT_DEATH(
      {
        const RB4 rb;
        const auto &crb = rb;
        (void)crb.front();
      },
      "");
}

TEST(FixedRingBufferDeathTest, ConstBackOnEmpty) {
  EXPECT_DEATH(
      {
        const RB4 rb;
        const auto &crb = rb;
        (void)crb.back();
      },
      "");
}

} // namespace
