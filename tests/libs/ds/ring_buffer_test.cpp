/**
 * @file ring_buffer_test.cpp
 * @brief Tests for mk::ds::RingBuffer — runtime-capacity ring buffer.
 */

#include "ds/ring_buffer.hpp"

#include <cstddef>
#include <numeric>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using mk::ds::RingBuffer;

// =============================================================================
// Test fixture — stack-allocated buffer, capacity 8
// =============================================================================

class RingBufferTest : public ::testing::Test {
protected:
  using RB = RingBuffer<int>;
  static constexpr std::size_t kCap = 8;

  void SetUp() override {
    auto opt = RB::create(buf_, sizeof(buf_), kCap);
    ASSERT_TRUE(opt.has_value());
    rb_ = std::move(*opt);  // NOLINT(bugprone-unchecked-optional-access)
  }

  alignas(64) std::byte buf_[RB::required_buffer_size(kCap)]{};
  RB rb_;
};

// =============================================================================
// 1. EmptyOnConstruction
// =============================================================================

TEST_F(RingBufferTest, EmptyOnConstruction) {
  EXPECT_EQ(rb_.size(), 0U);
  EXPECT_TRUE(rb_.empty());
  EXPECT_FALSE(rb_.full());
  EXPECT_EQ(rb_.capacity(), kCap);
}

// =============================================================================
// 2. PushAndAccess
// =============================================================================

TEST_F(RingBufferTest, PushAndAccess) {
  rb_.push(42);

  EXPECT_EQ(rb_.size(), 1U);
  EXPECT_FALSE(rb_.empty());
  EXPECT_FALSE(rb_.full());
  EXPECT_EQ(rb_.front(), 42);
  EXPECT_EQ(rb_.back(), 42);
  EXPECT_EQ(rb_[0], 42);
}

// =============================================================================
// 3. FIFOOrder
// =============================================================================

TEST_F(RingBufferTest, FIFOOrder) {
  for (int i = 0; i < 5; ++i) {
    rb_.push(i * 10);
  }

  EXPECT_EQ(rb_.size(), 5U);
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(rb_[i], static_cast<int>(i) * 10);
  }
  EXPECT_EQ(rb_.front(), 0);
  EXPECT_EQ(rb_.back(), 40);
}

// =============================================================================
// 4. OverwriteOldest
// =============================================================================

TEST_F(RingBufferTest, OverwriteOldest) {
  // Fill to capacity: [0, 1, 2, 3, 4, 5, 6, 7]
  for (int i = 0; std::cmp_less(i, kCap); ++i) {
    rb_.push(i);
  }
  ASSERT_TRUE(rb_.full());
  ASSERT_EQ(rb_.front(), 0);

  // Push one more — overwrites oldest (0). Buffer: [1, 2, ..., 8]
  rb_.push(static_cast<int>(kCap));
  EXPECT_TRUE(rb_.full());
  EXPECT_EQ(rb_.size(), kCap);
  EXPECT_EQ(rb_.front(), 1);
  EXPECT_EQ(rb_.back(), static_cast<int>(kCap));
  for (std::size_t i = 0; i < kCap; ++i) {
    EXPECT_EQ(rb_[i], static_cast<int>(i) + 1);
  }
}

// =============================================================================
// 5. OverwriteMultipleWraps
// =============================================================================

TEST_F(RingBufferTest, OverwriteMultipleWraps) {
  // Push 3 * capacity elements. Only the last 'capacity' should survive.
  constexpr int kTotal = static_cast<int>(3 * kCap);
  for (int i = 0; i < kTotal; ++i) {
    rb_.push(i);
  }

  EXPECT_TRUE(rb_.full());
  EXPECT_EQ(rb_.size(), kCap);

  // Last kCap elements: [16, 17, 18, 19, 20, 21, 22, 23]
  for (std::size_t i = 0; i < kCap; ++i) {
    EXPECT_EQ(rb_[i], kTotal - static_cast<int>(kCap) + static_cast<int>(i));
  }
}

// =============================================================================
// 6. PopFront
// =============================================================================

TEST_F(RingBufferTest, PopFront) {
  for (int i = 0; i < 5; ++i) {
    rb_.push(i * 10); // [0, 10, 20, 30, 40]
  }

  rb_.pop_front(); // [10, 20, 30, 40]
  EXPECT_EQ(rb_.size(), 4U);
  EXPECT_EQ(rb_.front(), 10);
  EXPECT_EQ(rb_[0], 10);
}

// =============================================================================
// 7. PopFrontToEmpty
// =============================================================================

TEST_F(RingBufferTest, PopFrontToEmpty) {
  rb_.push(1);
  rb_.push(2);
  rb_.push(3);

  rb_.pop_front();
  rb_.pop_front();
  rb_.pop_front();

  EXPECT_TRUE(rb_.empty());
  EXPECT_EQ(rb_.size(), 0U);
}

// =============================================================================
// 8. ClearResetsEverything
// =============================================================================

TEST_F(RingBufferTest, ClearResetsEverything) {
  for (int i = 0; i < 5; ++i) {
    rb_.push(i);
  }
  ASSERT_EQ(rb_.size(), 5U);

  rb_.clear();
  EXPECT_TRUE(rb_.empty());
  EXPECT_EQ(rb_.size(), 0U);
  EXPECT_FALSE(rb_.full());

  // Push again after clear — should work correctly.
  rb_.push(100);
  EXPECT_EQ(rb_.size(), 1U);
  EXPECT_EQ(rb_.front(), 100);
  EXPECT_EQ(rb_.back(), 100);
}

// =============================================================================
// 9. RangeForIteration
// =============================================================================

TEST_F(RingBufferTest, RangeForIteration) {
  for (int i = 0; i < 5; ++i) {
    rb_.push(i * 10);
  }

  // Range-for should yield elements in logical order (oldest to newest).
  std::vector<int> result;
  for (const auto &val : rb_) {
    result.push_back(val);
  }

  ASSERT_EQ(result.size(), 5U);
  for (std::size_t i = 0; i < result.size(); ++i) {
    EXPECT_EQ(result[i], rb_[i]);
  }
}

// =============================================================================
// 10. IteratorRandomAccess
// =============================================================================

TEST_F(RingBufferTest, IteratorRandomAccess) {
  for (int i = 0; i < 5; ++i) {
    rb_.push(i);
  }

  auto it_begin = rb_.begin();
  auto it_end = rb_.end();

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
  auto it = rb_.begin();
  EXPECT_EQ(*it, 0);
  ++it;
  EXPECT_EQ(*it, 1);
  auto prev = it++;
  EXPECT_EQ(*prev, 1);
  EXPECT_EQ(*it, 2);

  // STL algorithm compatibility: std::accumulate
  const int sum = std::accumulate(rb_.begin(), rb_.end(), 0);
  EXPECT_EQ(sum, 0 + 1 + 2 + 3 + 4);
}

// =============================================================================
// 11. ConstAccess
// =============================================================================

TEST_F(RingBufferTest, ConstAccess) {
  for (int i = 0; i < 5; ++i) {
    rb_.push(i * 10);
  }

  const auto &crb = rb_;

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
  const RingBuffer<int>::ConstIterator cit = rb_.begin();
  EXPECT_EQ(*cit, 0);
}

// =============================================================================
// 12. PushRvalueOverload
// =============================================================================

// Realistic HFT type: trivially destructible, default-constructible, POD.
struct Tick {
  int price;
  int quantity;

  bool operator==(const Tick &other) const noexcept = default;
};

TEST(RingBufferStandaloneTest, PushRvalueOverload) {
  using RB = RingBuffer<Tick>;
  constexpr std::size_t kCap = 4;
  alignas(64) std::byte buf[RB::required_buffer_size(kCap)]{};

  auto opt = RB::create(buf, sizeof(buf), kCap);
  ASSERT_TRUE(opt.has_value());
  auto rb = std::move(*opt);  // NOLINT(bugprone-unchecked-optional-access)

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

TEST_F(RingBufferTest, OverwriteWrapAround) {
  // Use a small capacity-4 buffer for clear wrap-around observation.
  using RB4 = RingBuffer<int>;
  constexpr std::size_t kCap4 = 4;
  alignas(64) std::byte buf4[RB4::required_buffer_size(kCap4)]{};

  auto opt = RB4::create(buf4, sizeof(buf4), kCap4);
  ASSERT_TRUE(opt.has_value());
  auto rb4 = std::move(*opt);  // NOLINT(bugprone-unchecked-optional-access)

  // Push 6: [0, 1, 2, 3, 4, 5]
  for (int i = 0; i < 6; ++i) {
    rb4.push(i);
  }

  EXPECT_TRUE(rb4.full());
  EXPECT_EQ(rb4.size(), kCap4);

  // Logical order: [2, 3, 4, 5]
  EXPECT_EQ(rb4[0], 2);
  EXPECT_EQ(rb4[1], 3);
  EXPECT_EQ(rb4[2], 4);
  EXPECT_EQ(rb4[3], 5);
  EXPECT_EQ(rb4.front(), 2);
  EXPECT_EQ(rb4.back(), 5);
}

// =============================================================================
// 14. LargeBufferStress
// =============================================================================

TEST(RingBufferStandaloneTest, LargeBufferStress) {
  using RB = RingBuffer<int>;
  constexpr std::size_t kCap = 1024;
  constexpr int kTotal = 10000;

  // Heap-allocate the large buffer (too big for stack in some configs).
  alignas(64) static std::byte buf[RB::required_buffer_size(kCap)]{};

  auto opt = RB::create(buf, sizeof(buf), kCap);
  ASSERT_TRUE(opt.has_value());
  auto rb = std::move(*opt);  // NOLINT(bugprone-unchecked-optional-access)

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
// Factory validation tests
// =============================================================================

TEST(RingBufferFactoryTest, FactoryWorks) {
  using RB = RingBuffer<int>;
  constexpr std::size_t kCap = 8;
  alignas(64) std::byte buf[RB::required_buffer_size(kCap)]{};

  auto opt = RB::create(buf, sizeof(buf), kCap);
  ASSERT_TRUE(opt.has_value());
  EXPECT_EQ(opt->capacity(), kCap);  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_TRUE(opt->empty());          // NOLINT(bugprone-unchecked-optional-access)
}

TEST(RingBufferFactoryTest, FactoryRejectsNull) {
  auto opt = RingBuffer<int>::create(nullptr, 64, 8);
  EXPECT_FALSE(opt.has_value());
}

TEST(RingBufferFactoryTest, FactoryRejectsSmallBuffer) {
  using RB = RingBuffer<int>;
  constexpr std::size_t kCap = 8;
  // Buffer too small — only half the required size.
  alignas(64) std::byte buf[RB::required_buffer_size(kCap) / 2]{};

  auto opt = RB::create(buf, sizeof(buf), kCap);
  EXPECT_FALSE(opt.has_value());
}

TEST(RingBufferFactoryTest, FactoryRejectsMisaligned) {
  using RB = RingBuffer<double>; // alignof(double) == 8
  constexpr std::size_t kCap = 4;

  // Allocate a large buffer, then offset by 1 byte to misalign.
  alignas(64) std::byte raw[RB::required_buffer_size(kCap) + 64]{};
  void *misaligned = raw + 1; // 1-byte offset breaks 8-byte alignment

  auto opt = RB::create(misaligned, sizeof(raw) - 1, kCap);
  EXPECT_FALSE(opt.has_value());
}

TEST(RingBufferFactoryTest, FactoryRejectsNonPowerOfTwo) {
  using RB = RingBuffer<int>;
  alignas(64) std::byte buf[1024]{};

  // Capacity 3 is not a power of two.
  auto opt = RB::create(buf, sizeof(buf), 3);
  EXPECT_FALSE(opt.has_value());

  // Capacity 1 is below minimum (2).
  auto opt2 = RB::create(buf, sizeof(buf), 1);
  EXPECT_FALSE(opt2.has_value());

  // Capacity 0.
  auto opt3 = RB::create(buf, sizeof(buf), 0);
  EXPECT_FALSE(opt3.has_value());
}

// =============================================================================
// Move semantics tests
// =============================================================================

TEST(RingBufferMoveTest, MoveConstructor) {
  using RB = RingBuffer<int>;
  constexpr std::size_t kCap = 4;
  alignas(64) std::byte buf[RB::required_buffer_size(kCap)]{};

  auto opt = RB::create(buf, sizeof(buf), kCap);
  ASSERT_TRUE(opt.has_value());
  auto src = std::move(*opt);  // NOLINT(bugprone-unchecked-optional-access)

  src.push(10);
  src.push(20);
  src.push(30);

  // Move-construct destination.
  RB dst(std::move(src));

  EXPECT_EQ(dst.size(), 3U);
  EXPECT_EQ(dst[0], 10);
  EXPECT_EQ(dst[1], 20);
  EXPECT_EQ(dst[2], 30);

  // Source is now default-state (capacity == 0).
  EXPECT_EQ(src.capacity(), 0U); // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(src.empty());
}

TEST(RingBufferMoveTest, MoveAssignment) {
  using RB = RingBuffer<int>;
  constexpr std::size_t kCap = 4;
  alignas(64) std::byte buf[RB::required_buffer_size(kCap)]{};

  auto opt = RB::create(buf, sizeof(buf), kCap);
  ASSERT_TRUE(opt.has_value());
  auto src = std::move(*opt);  // NOLINT(bugprone-unchecked-optional-access)

  src.push(10);
  src.push(20);

  // Move-assign to a default-constructed buffer.
  RB dst;
  dst = std::move(src);

  EXPECT_EQ(dst.size(), 2U);
  EXPECT_EQ(dst[0], 10);
  EXPECT_EQ(dst[1], 20);

  // Source is now default-state.
  EXPECT_EQ(src.capacity(), 0U); // NOLINT(bugprone-use-after-move)
}

// =============================================================================
// Static helper tests
// =============================================================================

TEST(RingBufferStaticTest, RoundUpCapacity) {
  using RB = RingBuffer<int>;

  EXPECT_EQ(RB::round_up_capacity(0), 0U); // Error case
  EXPECT_EQ(RB::round_up_capacity(1), 2U); // Minimum is 2
  EXPECT_EQ(RB::round_up_capacity(2), 2U);
  EXPECT_EQ(RB::round_up_capacity(3), 4U);
  EXPECT_EQ(RB::round_up_capacity(4), 4U);
  EXPECT_EQ(RB::round_up_capacity(5), 8U);
  EXPECT_EQ(RB::round_up_capacity(7), 8U);
  EXPECT_EQ(RB::round_up_capacity(8), 8U);
  EXPECT_EQ(RB::round_up_capacity(1023), 1024U);
  EXPECT_EQ(RB::round_up_capacity(1024), 1024U);
}

TEST(RingBufferStaticTest, RequiredBufferSize) {
  using RB = RingBuffer<int>;
  EXPECT_EQ(RB::required_buffer_size(8), 8 * sizeof(int));
  EXPECT_EQ(RB::required_buffer_size(1024), 1024 * sizeof(int));

  using RBD = RingBuffer<double>;
  EXPECT_EQ(RBD::required_buffer_size(8), 8 * sizeof(double));
  EXPECT_EQ(RBD::element_size(), sizeof(double));
}

// =============================================================================
// Default-constructed safety
// =============================================================================

TEST(RingBufferSpecialTest, DefaultConstructedIsSafe) {
  RingBuffer<int> rb;

  EXPECT_EQ(rb.capacity(), 0U);
  EXPECT_EQ(rb.size(), 0U);
  EXPECT_TRUE(rb.empty());
  EXPECT_TRUE(rb.full()); // capacity == 0, count == 0, so 0 == 0 → full

  // push on capacity-0 is a no-op.
  rb.push(42);
  EXPECT_EQ(rb.size(), 0U);
  EXPECT_TRUE(rb.empty());

  // clear on capacity-0 is safe.
  rb.clear();
  EXPECT_EQ(rb.size(), 0U);
}

// =============================================================================
// Direct constructor test
// =============================================================================

TEST(RingBufferSpecialTest, DirectConstructorWorks) {
  using RB = RingBuffer<int>;
  constexpr std::size_t kCap = 4;
  alignas(64) std::byte buf[RB::required_buffer_size(kCap)]{};

  // Direct constructor — startup-time use, aborts on invalid.
  RB rb(buf, sizeof(buf), kCap);

  EXPECT_EQ(rb.capacity(), kCap);
  EXPECT_TRUE(rb.empty());

  rb.push(10);
  rb.push(20);
  EXPECT_EQ(rb.size(), 2U);
  EXPECT_EQ(rb.front(), 10);
  EXPECT_EQ(rb.back(), 20);
}

// =============================================================================
// Death tests — precondition violations abort
// =============================================================================

TEST(RingBufferDeathTest, DirectConstructorAbortsOnNonPowerOfTwo) {
  EXPECT_DEATH(
      {
        alignas(64) std::byte buf[128]{};
        const RingBuffer<int> rb(buf, sizeof(buf), 3);
      },
      "");
}

TEST(RingBufferDeathTest, DirectConstructorAbortsOnNull) {
  EXPECT_DEATH({ const RingBuffer<int> rb(nullptr, 64, 8); }, "");
}

TEST(RingBufferDeathTest, FrontOnEmpty) {
  EXPECT_DEATH(
      {
        using RB = RingBuffer<int>;
        alignas(64) std::byte buf[RB::required_buffer_size(4)]{};
        auto opt = RB::create(buf, sizeof(buf), 4);
        (void)opt->front();  // NOLINT(bugprone-unchecked-optional-access)
      },
      "");
}

TEST(RingBufferDeathTest, BackOnEmpty) {
  EXPECT_DEATH(
      {
        using RB = RingBuffer<int>;
        alignas(64) std::byte buf[RB::required_buffer_size(4)]{};
        auto opt = RB::create(buf, sizeof(buf), 4);
        (void)opt->back();  // NOLINT(bugprone-unchecked-optional-access)
      },
      "");
}

TEST(RingBufferDeathTest, PopFrontOnEmpty) {
  EXPECT_DEATH(
      {
        using RB = RingBuffer<int>;
        alignas(64) std::byte buf[RB::required_buffer_size(4)]{};
        auto opt = RB::create(buf, sizeof(buf), 4);
        opt->pop_front();  // NOLINT(bugprone-unchecked-optional-access)
      },
      "");
}

TEST(RingBufferDeathTest, SubscriptOutOfBounds) {
  EXPECT_DEATH(
      {
        using RB = RingBuffer<int>;
        alignas(64) std::byte buf[RB::required_buffer_size(4)]{};
        auto opt = RB::create(buf, sizeof(buf), 4);
        opt->push(42);    // NOLINT(bugprone-unchecked-optional-access)
        (void)(*opt)[1]; // size() == 1, index 1 is out of bounds  // NOLINT(bugprone-unchecked-optional-access)
      },
      "");
}

} // namespace
