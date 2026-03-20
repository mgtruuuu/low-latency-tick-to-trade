/**
 * @file aligned_new_allocator_test.cpp
 * @brief GTest-based tests for AlignedNewAllocator.
 *
 * Note: AlignedNewAllocator is deprecated in C++20 (std::allocator handles
 * over-alignment automatically). These tests are kept to verify the
 * implementation still works correctly as educational reference.
 *
 * Test plan:
 *   1. Over-aligned type (alignas(64)) allocation is correctly aligned
 *   2. Standard type (int) allocation works
 *   3. Multiple elements maintain alignment and data integrity
 */

// Suppress deprecation warnings — we are intentionally testing deprecated code.
// The [[deprecated]] attribute is only active in C++20 mode.
#if __cplusplus >= 202002L
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "memory/aligned_new_allocator.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

// ============================================================================
// Test Data Type
// ============================================================================
//
// In HFT, aligning hot data structures to cache-line (64 bytes) boundaries
// is standard practice. alignas(64) requests the compiler to enforce 64-byte
// alignment. AlignedNewAllocator preserves this requirement on heap as well.

struct alignas(64) Order {
  std::uint64_t id;
  double price;
  double quantity;
  char padding[40]; // Explicit padding to fill the full 64-byte cache line
};

// ============================================================================
// Test Fixture
// ============================================================================
//
// Provides a pre-configured vector using AlignedNewAllocator<Order>.
// Each TEST_F creates a fresh Fixture instance, ensuring test isolation.

class AlignedNewAllocatorTest : public ::testing::Test {
protected:
  using AlignedOrderVector =
      std::vector<Order, mk::sys::memory::AlignedNewAllocator<Order>>;

  AlignedOrderVector order_book_;
};

// ============================================================================
// 1. Over-Aligned Allocation Alignment Check
// ============================================================================
//
// Core feature of AlignedNewAllocator: when alignof(T) exceeds the default
// alignment, it delegates to C++17 aligned operator new to guarantee correct
// alignment on the heap.

TEST_F(AlignedNewAllocatorTest, OverAlignedAllocationIsCorrectlyAligned) {
  // reserve: internally calls aligned operator new
  order_book_.reserve(1024);
  order_book_.emplace_back(
      Order{.id = 1, .price = 100.5, .quantity = 10.0, .padding = {}});

  auto *ptr = order_book_.data();
  auto address = reinterpret_cast<std::uintptr_t>(ptr);

  // Verify address is a multiple of alignof(Order) = 64
  EXPECT_EQ(0U, address % alignof(Order))
      << "Memory at 0x" << std::hex << address << " is not aligned to "
      << std::dec << alignof(Order) << " bytes";
}

// ============================================================================
// 2. Standard Type (int) Allocation
// ============================================================================
//
// int has default alignment (typically 4 bytes), so AlignedNewAllocator
// takes the "default path" (plain operator new). Verify this path works too.

TEST(AlignedNewAllocatorBasic, StandardTypeAllocationWorks) {
  std::vector<int, mk::sys::memory::AlignedNewAllocator<int>> int_vec;
  int_vec.push_back(42);
  int_vec.push_back(99);

  EXPECT_EQ(2U, int_vec.size());
  EXPECT_EQ(42, int_vec[0]);
  EXPECT_EQ(99, int_vec[1]);
}

// ============================================================================
// 3. Multiple Elements Maintain Alignment & Data Integrity
// ============================================================================
//
// After reserve + multiple emplace_back calls:
//   - data() start address must be 64-byte aligned
//   - Since sizeof(Order) == 64, each element is naturally aligned too
//   - Data values must be correctly preserved

TEST_F(AlignedNewAllocatorTest, MultipleElementsMaintainAlignmentAndData) {
  order_book_.reserve(100);
  for (std::uint64_t i = 0; i < 10; ++i) {
    order_book_.emplace_back(
        Order{.id = i,
              .price = 100.0 + static_cast<double>(i),
              .quantity = static_cast<double>(i),
              .padding = {}});
  }

  // Verify start address alignment
  auto address = reinterpret_cast<std::uintptr_t>(order_book_.data());
  EXPECT_EQ(0U, address % alignof(Order));

  // Verify data integrity
  for (std::uint64_t i = 0; i < 10; ++i) {
    EXPECT_EQ(i, order_book_[i].id);
    EXPECT_DOUBLE_EQ(100.0 + static_cast<double>(i), order_book_[i].price);
  }
}

#if __cplusplus >= 202002L
#pragma GCC diagnostic pop
#endif
