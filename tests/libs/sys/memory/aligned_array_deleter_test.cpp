/**
 * @file aligned_array_deleter_test.cpp
 * @brief Tests for AlignedArrayDeleter and AlignedByteArray alias.
 *
 * Test plan:
 *   1. AlignedByteArray constructs and destructs without ASan errors
 *   2. Allocated memory is correctly aligned
 *   3. Buffer contents are readable and writable
 *   4. Default alignment (max_align_t) works
 *   5. Move semantics work correctly
 */

#include "sys/memory/aligned_array_deleter.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include <gtest/gtest.h>

namespace {

using mk::sys::memory::AlignedByteArray;

// =============================================================================
// 1. Construction and destruction — ASan validates new/delete pairing
// =============================================================================

TEST(AlignedArrayDeleterTest, ConstructAndDestruct) {
  constexpr std::size_t kAlign = 64;
  constexpr std::size_t kSize = 1024;

  // If new/delete alignment is mismatched, ASan will catch it.
  AlignedByteArray<kAlign> const buf{new (std::align_val_t{kAlign})
                                         std::byte[kSize]};
  ASSERT_NE(buf.get(), nullptr);
}

// =============================================================================
// 2. Allocated memory is correctly aligned
// =============================================================================

TEST(AlignedArrayDeleterTest, MemoryIsAligned) {
  constexpr std::size_t kAlign = 64;
  constexpr std::size_t kSize = 256;

  AlignedByteArray<kAlign> const buf{new (std::align_val_t{kAlign})
                                         std::byte[kSize]};

  auto addr = reinterpret_cast<std::uintptr_t>(buf.get());
  EXPECT_EQ(addr % kAlign, 0U)
      << "Buffer at 0x" << std::hex << addr << " is not " << std::dec << kAlign
      << "-byte aligned";
}

// =============================================================================
// 3. Buffer contents are readable and writable
// =============================================================================

TEST(AlignedArrayDeleterTest, ReadWriteContents) {
  constexpr std::size_t kAlign = 64;
  constexpr std::size_t kSize = 128;

  AlignedByteArray<kAlign> const buf{new (std::align_val_t{kAlign})
                                         std::byte[kSize]};

  // Write a known pattern.
  std::memset(buf.get(), 0xAB, kSize);

  // Verify all bytes.
  for (std::size_t i = 0; i < kSize; ++i) {
    ASSERT_EQ(buf[i], std::byte{0xAB});
  }
}

// =============================================================================
// 4. Default alignment (max_align_t) works
// =============================================================================

TEST(AlignedArrayDeleterTest, DefaultAlignment) {
  constexpr std::size_t kSize = 512;

  // AlignedByteArray<> defaults to alignof(std::max_align_t).
  AlignedByteArray<> const buf{new (std::align_val_t{alignof(std::max_align_t)})
                                   std::byte[kSize]};
  ASSERT_NE(buf.get(), nullptr);

  auto addr = reinterpret_cast<std::uintptr_t>(buf.get());
  EXPECT_EQ(addr % alignof(std::max_align_t), 0U);
}

// =============================================================================
// 5. Move semantics
// =============================================================================

TEST(AlignedArrayDeleterTest, MoveTransfersOwnership) {
  constexpr std::size_t kAlign = 64;
  constexpr std::size_t kSize = 256;

  AlignedByteArray<kAlign> a{new (std::align_val_t{kAlign}) std::byte[kSize]};
  auto *raw = a.get();
  ASSERT_NE(raw, nullptr);

  // Move ownership from a to b.
  AlignedByteArray<kAlign> const b = std::move(a);
  EXPECT_EQ(a.get(), nullptr);
  EXPECT_EQ(b.get(), raw);
}

} // namespace
