/**
 * @file index_free_stack_test.cpp
 * @brief Tests for mk::ds::IndexFreeStack (runtime-capacity variant).
 */

#include "ds/index_free_stack.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <set>
#include <vector>

#include <gtest/gtest.h>

namespace {

using mk::ds::IndexFreeStack;

// =============================================================================
// Helper: allocate an aligned buffer for IndexFreeStack
// =============================================================================

constexpr std::size_t kDefaultCapacity = 32;

/// Fixture that allocates an aligned buffer and constructs an IndexFreeStack
/// via the create() factory.
class IndexFreeStackTest : public ::testing::Test {
protected:
  static constexpr std::size_t kCap = kDefaultCapacity;
  static constexpr auto kAlign = std::align_val_t{alignof(std::uint32_t)};

  std::size_t buf_size_ = IndexFreeStack::required_buffer_size(kCap);
  std::byte *buf_ = new (kAlign) std::byte[buf_size_];

  IndexFreeStack stack_ = [&]() {
    auto opt = IndexFreeStack::create(buf_, buf_size_, kCap);
    if (!opt) {
      std::abort();
    }
    return std::move(*opt);  // NOLINT(bugprone-unchecked-optional-access)
  }();

  IndexFreeStackTest() = default;
  ~IndexFreeStackTest() override { ::operator delete[](buf_, kAlign); }

  IndexFreeStackTest(const IndexFreeStackTest &) = delete;            // NOLINT(modernize-use-equals-delete)
  IndexFreeStackTest &operator=(const IndexFreeStackTest &) = delete; // NOLINT(modernize-use-equals-delete)
  IndexFreeStackTest(IndexFreeStackTest &&) = delete;                 // NOLINT(modernize-use-equals-delete)
  IndexFreeStackTest &operator=(IndexFreeStackTest &&) = delete;      // NOLINT(modernize-use-equals-delete)
};

// =============================================================================
// 1. Construction
// =============================================================================

TEST(IndexFreeStackCreateTest, FactorySuccess) {
  constexpr std::size_t kCap = 16;
  const auto buf_size = IndexFreeStack::required_buffer_size(kCap);
  auto *buf =
      new (std::align_val_t{alignof(std::uint32_t)}) std::byte[buf_size];

  auto opt = IndexFreeStack::create(buf, buf_size, kCap);
  ASSERT_TRUE(opt.has_value());
  EXPECT_EQ(opt->capacity(), kCap);    // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(opt->available(), kCap);   // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_TRUE(opt->full());           // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_FALSE(opt->empty());         // NOLINT(bugprone-unchecked-optional-access)

  ::operator delete[](buf, std::align_val_t{alignof(std::uint32_t)});
}

TEST(IndexFreeStackCreateTest, NullptrReturnsNullopt) {
  EXPECT_FALSE(IndexFreeStack::create(nullptr, 1024, 8).has_value());
}

TEST(IndexFreeStackCreateTest, UndersizedBufferReturnsNullopt) {
  constexpr std::size_t kCap = 16;
  const auto needed = IndexFreeStack::required_buffer_size(kCap);
  auto *buf = new std::byte[needed];

  // One byte short.
  EXPECT_FALSE(IndexFreeStack::create(buf, needed - 1, kCap).has_value());

  delete[] buf;
}

TEST(IndexFreeStackCreateTest, ZeroCapacityReturnsNullopt) {
  std::byte buf[64]{};
  EXPECT_FALSE(IndexFreeStack::create(buf, sizeof(buf), 0).has_value());
}

TEST(IndexFreeStackCreateTest, MisalignedBufferReturnsNullopt) {
  // Allocate extra byte and offset by 1 to misalign for uint32_t.
  constexpr std::size_t kCap = 8;
  const auto needed = IndexFreeStack::required_buffer_size(kCap);
  auto *raw = new std::byte[needed + alignof(std::uint32_t)];

  // Find a misaligned address.
  auto *misaligned = raw;
  if (reinterpret_cast<std::uintptr_t>(misaligned) % alignof(std::uint32_t) ==
      0) {
    misaligned += 1;
  }

  EXPECT_FALSE(IndexFreeStack::create(misaligned, needed, kCap).has_value());
  delete[] raw;
}

TEST(IndexFreeStackCreateTest, DirectConstructorSuccess) {
  constexpr std::size_t kCap = 8;
  const auto buf_size = IndexFreeStack::required_buffer_size(kCap);
  auto *buf =
      new (std::align_val_t{alignof(std::uint32_t)}) std::byte[buf_size];

  const IndexFreeStack stack(buf, buf_size, kCap);
  EXPECT_EQ(stack.capacity(), kCap);
  EXPECT_EQ(stack.available(), kCap);
  EXPECT_TRUE(stack.full());

  ::operator delete[](buf, std::align_val_t{alignof(std::uint32_t)});
}

TEST(IndexFreeStackCreateTest, DefaultConstructorCreatesUnusableStack) {
  const IndexFreeStack stack;
  EXPECT_EQ(stack.capacity(), 0U);
  EXPECT_EQ(stack.available(), 0U);
  EXPECT_TRUE(stack.empty());
  EXPECT_TRUE(stack.full()); // 0 == 0
}

// =============================================================================
// 2. Static helpers
// =============================================================================

TEST(IndexFreeStackCreateTest, RequiredBufferSize) {
  // Release: kCap × sizeof(uint32_t)
  // Debug: kCap × sizeof(uint32_t) + kCap × sizeof(bool)
  constexpr std::size_t kCap = 64;
  const auto size = IndexFreeStack::required_buffer_size(kCap);
#ifdef NDEBUG
  EXPECT_EQ(size, kCap * sizeof(std::uint32_t));
#else
  EXPECT_EQ(size, (kCap * sizeof(std::uint32_t)) + (kCap * sizeof(bool)));
#endif
}

// =============================================================================
// 3. Death tests
// =============================================================================

TEST(IndexFreeStackDeathTest, DirectConstructorAbortsOnNullptr) {
  EXPECT_DEATH({ const IndexFreeStack stack(nullptr, 1024, 8); }, "");
}

TEST(IndexFreeStackDeathTest, DirectConstructorAbortsOnZeroCapacity) {
  std::byte buf[64]{};
  EXPECT_DEATH({ const IndexFreeStack stack(buf, sizeof(buf), 0); }, "");
}

TEST(IndexFreeStackDeathTest, PushOnFullStackAborts) {
  EXPECT_DEATH(
      {
        constexpr std::size_t kCap = 4;
        auto buf_size = IndexFreeStack::required_buffer_size(kCap);
        auto *buf =
            new (std::align_val_t{alignof(std::uint32_t)}) std::byte[buf_size];
        IndexFreeStack stack(buf, buf_size, kCap);
        // Stack is full. Push without pop.
        stack.push(0);
      },
      "");
}

TEST(IndexFreeStackDeathTest, PushOutOfRangeIndexAborts) {
  EXPECT_DEATH(
      {
        constexpr std::size_t kCap = 4;
        auto buf_size = IndexFreeStack::required_buffer_size(kCap);
        auto *buf =
            new (std::align_val_t{alignof(std::uint32_t)}) std::byte[buf_size];
        IndexFreeStack stack(buf, buf_size, kCap);
        (void)stack.pop();
        stack.push(4); // idx == capacity → out of range.
      },
      "");
}

#ifndef NDEBUG
TEST(IndexFreeStackDeathTest, DoubleFreeAborts) {
  EXPECT_DEATH(
      {
        constexpr std::size_t kCap = 4;
        auto buf_size = IndexFreeStack::required_buffer_size(kCap);
        auto *buf =
            new (std::align_val_t{alignof(std::uint32_t)}) std::byte[buf_size];
        IndexFreeStack stack(buf, buf_size, kCap);
        auto idx = stack.pop();
        stack.push(*idx);  // NOLINT(bugprone-unchecked-optional-access)
        stack.push(*idx); // Already returned — double-free.  // NOLINT(bugprone-unchecked-optional-access)
      },
      "");
}

TEST(IndexFreeStackDeathTest, PushNeverPoppedIndexAborts) {
  EXPECT_DEATH(
      {
        constexpr std::size_t kCap = 4;
        auto buf_size = IndexFreeStack::required_buffer_size(kCap);
        auto *buf =
            new (std::align_val_t{alignof(std::uint32_t)}) std::byte[buf_size];
        IndexFreeStack stack(buf, buf_size, kCap);
        auto popped = stack.pop();
        const std::uint32_t other = (*popped + 1) % 4;  // NOLINT(bugprone-unchecked-optional-access)
        stack.push(other);
      },
      "");
}
#endif // NDEBUG

// =============================================================================
// 4. Core operations (fixture-based)
// =============================================================================

TEST_F(IndexFreeStackTest, StartsFullWithAllIndices) {
  EXPECT_EQ(stack_.available(), kCap);
  EXPECT_EQ(stack_.capacity(), kCap);
  EXPECT_TRUE(stack_.full());
  EXPECT_FALSE(stack_.empty());
}

TEST_F(IndexFreeStackTest, PopReturnsUniqueIndices) {
  std::set<std::uint32_t> seen;
  for (std::size_t i = 0; i < kCap; ++i) {
    auto idx = stack_.pop();
    ASSERT_TRUE(idx.has_value());
    EXPECT_LT(*idx, kCap);                                          // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_TRUE(seen.insert(*idx).second) << "Duplicate index: " << *idx;  // NOLINT(bugprone-unchecked-optional-access)
  }
  EXPECT_TRUE(stack_.empty());
  EXPECT_EQ(stack_.available(), 0U);
}

TEST_F(IndexFreeStackTest, PopFromEmptyReturnsNullopt) {
  // Drain the stack.
  for (std::size_t i = 0; i < kCap; ++i) {
    (void)stack_.pop();
  }
  EXPECT_EQ(stack_.pop(), std::nullopt);
  EXPECT_TRUE(stack_.empty());
}

TEST_F(IndexFreeStackTest, PushRecyclesIndex) {
  auto idx = stack_.pop();
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(stack_.available(), kCap - 1);

  stack_.push(*idx);  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(stack_.available(), kCap);
  EXPECT_TRUE(stack_.full());
}

TEST_F(IndexFreeStackTest, LIFOOrdering) {
  auto a = stack_.pop();
  auto b = stack_.pop();
  auto c = stack_.pop();

  stack_.push(*a);  // NOLINT(bugprone-unchecked-optional-access)
  stack_.push(*b);  // NOLINT(bugprone-unchecked-optional-access)
  stack_.push(*c);  // NOLINT(bugprone-unchecked-optional-access)

  // LIFO: should get c, b, a back.
  EXPECT_EQ(stack_.pop(), c);
  EXPECT_EQ(stack_.pop(), b);
  EXPECT_EQ(stack_.pop(), a);
}

TEST_F(IndexFreeStackTest, ExhaustAndRefill) {
  std::vector<std::uint32_t> indices;
  for (std::size_t i = 0; i < kCap; ++i) {
    auto idx = stack_.pop();
    ASSERT_TRUE(idx.has_value());
    indices.push_back(*idx);  // NOLINT(bugprone-unchecked-optional-access)
  }
  EXPECT_TRUE(stack_.empty());

  for (auto idx : indices) {
    stack_.push(idx);
  }
  EXPECT_TRUE(stack_.full());
  EXPECT_EQ(stack_.available(), kCap);
}

TEST_F(IndexFreeStackTest, CapacityOne) {
  // Create a separate capacity-1 stack.
  constexpr std::size_t kCap1 = 1;
  const auto size1 = IndexFreeStack::required_buffer_size(kCap1);
  auto *buf1 = new (std::align_val_t{alignof(std::uint32_t)}) std::byte[size1];
  auto opt = IndexFreeStack::create(buf1, size1, kCap1);
  ASSERT_TRUE(opt.has_value());
  auto &s = *opt;  // NOLINT(bugprone-unchecked-optional-access)

  EXPECT_EQ(s.available(), 1U);

  auto idx = s.pop();
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 0U);  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_TRUE(s.empty());

  EXPECT_EQ(s.pop(), std::nullopt);

  s.push(0);
  EXPECT_TRUE(s.full());

  ::operator delete[](buf1, std::align_val_t{alignof(std::uint32_t)});
}

// =============================================================================
// 5. Move semantics
// =============================================================================

TEST_F(IndexFreeStackTest, MoveConstruction) {
  // Pop a few to create non-trivial state.
  (void)stack_.pop();
  (void)stack_.pop();
  const auto avail_before = stack_.available();

  const IndexFreeStack moved(std::move(stack_));
  EXPECT_EQ(moved.available(), avail_before);
  EXPECT_EQ(moved.capacity(), kCap);

  // Moved-from should be in default state.
  EXPECT_EQ(stack_.capacity(), 0U);
  EXPECT_EQ(stack_.available(), 0U);
}

TEST_F(IndexFreeStackTest, MoveAssignment) {
  (void)stack_.pop();
  (void)stack_.pop();
  const auto avail_before = stack_.available();

  IndexFreeStack target;
  target = std::move(stack_);
  EXPECT_EQ(target.available(), avail_before);
  EXPECT_EQ(target.capacity(), kCap);

  EXPECT_EQ(stack_.capacity(), 0U);
}

TEST_F(IndexFreeStackTest, MovedFromIsDefaultState) {
  const IndexFreeStack moved(std::move(stack_));
  EXPECT_EQ(stack_.capacity(), 0U);
  EXPECT_EQ(stack_.available(), 0U);
  EXPECT_TRUE(stack_.empty());
  EXPECT_TRUE(stack_.full()); // 0 == 0
}

// =============================================================================
// 6. Non-power-of-two capacity
// =============================================================================

TEST(IndexFreeStackCreateTest, NonPowerOfTwoCapacity) {
  // IndexFreeStack does NOT require power-of-two capacity (unlike ring
  // buffers).
  constexpr std::size_t kCap = 13;
  const auto buf_size = IndexFreeStack::required_buffer_size(kCap);
  auto *buf =
      new (std::align_val_t{alignof(std::uint32_t)}) std::byte[buf_size];

  auto opt = IndexFreeStack::create(buf, buf_size, kCap);
  ASSERT_TRUE(opt.has_value());
  EXPECT_EQ(opt->capacity(), kCap);    // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(opt->available(), kCap);   // NOLINT(bugprone-unchecked-optional-access)

  // Verify all 13 indices are unique and in [0, 13).
  std::set<std::uint32_t> seen;
  for (std::size_t i = 0; i < kCap; ++i) {
    auto idx = opt->pop();            // NOLINT(bugprone-unchecked-optional-access)
    ASSERT_TRUE(idx.has_value());
    EXPECT_LT(*idx, kCap);             // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_TRUE(seen.insert(*idx).second);  // NOLINT(bugprone-unchecked-optional-access)
  }
  EXPECT_TRUE(opt->empty());          // NOLINT(bugprone-unchecked-optional-access)

  ::operator delete[](buf, std::align_val_t{alignof(std::uint32_t)});
}

// =============================================================================
// 7. Large capacity
// =============================================================================

TEST(IndexFreeStackCreateTest, LargeCapacity) {
  constexpr std::size_t kCap = 10'000;
  const auto buf_size = IndexFreeStack::required_buffer_size(kCap);
  auto *buf =
      new (std::align_val_t{alignof(std::uint32_t)}) std::byte[buf_size];

  auto opt = IndexFreeStack::create(buf, buf_size, kCap);
  ASSERT_TRUE(opt.has_value());
  EXPECT_EQ(opt->capacity(), kCap);  // NOLINT(bugprone-unchecked-optional-access)

  // Pop all, push all back.
  std::vector<std::uint32_t> indices;
  indices.reserve(kCap);
  for (std::size_t i = 0; i < kCap; ++i) {
    auto idx = opt->pop();            // NOLINT(bugprone-unchecked-optional-access)
    ASSERT_TRUE(idx.has_value());
    indices.push_back(*idx);           // NOLINT(bugprone-unchecked-optional-access)
  }
  EXPECT_TRUE(opt->empty());          // NOLINT(bugprone-unchecked-optional-access)

  for (auto idx : indices) {
    opt->push(idx);                    // NOLINT(bugprone-unchecked-optional-access)
  }
  EXPECT_TRUE(opt->full());           // NOLINT(bugprone-unchecked-optional-access)

  ::operator delete[](buf, std::align_val_t{alignof(std::uint32_t)});
}

} // namespace
