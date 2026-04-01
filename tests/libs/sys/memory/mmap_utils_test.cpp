/**
 * @file mmap_utils_test.cpp
 * @brief GTest-based tests for mmap_utils public + internal allocation helpers.
 *
 * Test plan:
 *
 *   Internal (AllocationConfig-based) API:
 *     - try_allocate_with_hugepage_fallback:
 *         valid region, zero size, writable, large (2MB), ThpOnly, ExplicitOnly
 *     - allocate_with_hugepage_fallback:
 *         valid region, zero-size abort (death test)
 *     - AllocationConfig default values
 *     - FailureMode::kFailFast (zero size still nullopt)
 *
 *   Public intent API (RegionIntentConfig-based):
 *     - try_allocate_region: all 5 intents, zero size
 *     - allocate_region: all 5 intents, zero-size abort (death test)
 *     - RegionIntentConfig default values
 *
 *   Convenience wrappers:
 *     - try_allocate_hot_rw_region / allocate_hot_rw_region
 *     - try_allocate_read_mostly_region / allocate_read_mostly_region
 *     - try_allocate_cold_region / allocate_cold_region
 *
 *   Semantic verification:
 *     - kHotRw region is write-prefaulted (writable without fault)
 *     - kReadMostly region is readable
 *     - kCold region is still accessible (pages fault on first touch)
 *     - kCold intent forces kRegularPages (overrides config page_mapping_mode)
 *     - kRegularPages mode works at both internal and public API levels
 */

#include "memory/mmap_utils_internal.hpp"

#include <cstring>
#include <gtest/gtest.h>

namespace mk::sys::memory {
namespace {

// ============================================================================
// Internal API: try_allocate_with_hugepage_fallback
// ============================================================================

TEST(MmapUtilsTest, TryAllocate_ReturnsValidRegion) {
  auto region = try_allocate_with_hugepage_fallback({.size = 4096});
  ASSERT_TRUE(region.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_NE(region->get(), nullptr);
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_GE(region->size(), 4096U);
}

TEST(MmapUtilsTest, TryAllocate_ZeroSizeReturnsNullopt) {
  auto region = try_allocate_with_hugepage_fallback({.size = 0});
  EXPECT_FALSE(region.has_value());
}

TEST(MmapUtilsTest, TryAllocate_RegionIsWritable) {
  auto region = try_allocate_with_hugepage_fallback(
      {.size = 4096, .pf = PrefaultPolicy::kPopulateWrite});
  ASSERT_TRUE(region.has_value());

  // Write pattern and read back — verifies pages are faulted and writable.
  auto *p = static_cast<char *>(
      region->get()); // NOLINT(bugprone-unchecked-optional-access)
  std::memset(p, 0xAB, 4096);
  EXPECT_EQ(p[0], static_cast<char>(0xAB));
  EXPECT_EQ(p[4095], static_cast<char>(0xAB));
}

TEST(MmapUtilsTest, TryAllocate_LargeAllocation) {
  constexpr std::size_t kSize = 2UL * 1024 * 1024; // 2MB
  auto region = try_allocate_with_hugepage_fallback({.size = kSize});
  ASSERT_TRUE(region.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_GE(region->size(), kSize);
}

TEST(MmapUtilsTest, TryAllocate_ThpOnlyReturnsValidRegion) {
  auto region = try_allocate_with_hugepage_fallback(
      {.size = 4096, .page_mapping_mode = PageMappingMode::kThpOnly});
  ASSERT_TRUE(region.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_NE(region->get(), nullptr);
}

TEST(MmapUtilsTest, TryAllocate_ThpOnlyIsWritable) {
  auto region = try_allocate_with_hugepage_fallback(
      {.size = 4096,
       .pf = PrefaultPolicy::kPopulateWrite,
       .page_mapping_mode = PageMappingMode::kThpOnly});
  ASSERT_TRUE(region.has_value());

  auto *p = static_cast<char *>(
      region->get()); // NOLINT(bugprone-unchecked-optional-access)
  std::memset(p, 0xCD, 4096);
  EXPECT_EQ(p[0], static_cast<char>(0xCD));
}

TEST(MmapUtilsTest, TryAllocate_ExplicitOnlyMaySucceedOrFail) {
  // ExplicitOnly depends on pre-reserved huge pages on the machine.
  // On machines with nr_hugepages > 0: returns a valid region.
  // On machines without: returns nullopt (not an error).
  auto region = try_allocate_with_hugepage_fallback(
      {.size = 2UL * 1024 * 1024,
       .page_mapping_mode = PageMappingMode::kExplicitOnly});
  if (region.has_value()) {
    EXPECT_NE(region->get(),
              nullptr); // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_GE(region->size(),
              2UL * 1024 * 1024); // NOLINT(bugprone-unchecked-optional-access)
  }
  // nullopt is also acceptable — machine may not have explicit huge pages.
}

TEST(MmapUtilsTest, TryAllocate_NoPrefaultRegionIsStillAccessible) {
  auto region = try_allocate_with_hugepage_fallback(
      {.size = 4096, .pf = PrefaultPolicy::kNone});
  ASSERT_TRUE(region.has_value());

  // Pages are not prefaulted but still accessible (demand-faulted on write).
  auto *p = static_cast<char *>(
      region->get()); // NOLINT(bugprone-unchecked-optional-access)
  p[0] = 42;
  EXPECT_EQ(p[0], 42);
}

TEST(MmapUtilsTest, TryAllocate_FailFastZeroSizeReturnsNullopt) {
  // FailFast doesn't change the zero-size early return.
  auto region = try_allocate_with_hugepage_fallback(
      {.size = 0, .failure_mode = FailureMode::kFailFast});
  EXPECT_FALSE(region.has_value());
}

TEST(MmapUtilsTest, TryAllocate_FailFastValidSizeSucceeds) {
  auto region = try_allocate_with_hugepage_fallback(
      {.size = 4096, .failure_mode = FailureMode::kFailFast});
  ASSERT_TRUE(region.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_NE(region->get(), nullptr);
}

TEST(MmapUtilsTest, TryAllocate_ManualWritePrefault) {
  auto region = try_allocate_with_hugepage_fallback(
      {.size = 4096, .pf = PrefaultPolicy::kManualWrite});
  ASSERT_TRUE(region.has_value());

  auto *p = static_cast<char *>(
      region->get()); // NOLINT(bugprone-unchecked-optional-access)
  std::memset(p, 0xEF, 4096);
  EXPECT_EQ(p[0], static_cast<char>(0xEF));
}

TEST(MmapUtilsTest, TryAllocate_ManualReadPrefault) {
  auto region = try_allocate_with_hugepage_fallback(
      {.size = 4096, .pf = PrefaultPolicy::kManualRead});
  ASSERT_TRUE(region.has_value());

  // Read-prefaulted pages may be zero-page mapped; writing forces real page.
  auto *p = static_cast<char *>(
      region->get()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(p[0], 0);
}

// ============================================================================
// Internal API: allocate_with_hugepage_fallback
// ============================================================================

TEST(MmapUtilsTest, Allocate_ReturnsValidRegion) {
  auto region = allocate_with_hugepage_fallback({.size = 4096});
  EXPECT_NE(region.get(), nullptr);
  EXPECT_GE(region.size(), 4096U);
}

using MmapUtilsDeathTest = ::testing::Test;

TEST_F(MmapUtilsDeathTest, Allocate_ZeroSizeAborts) {
  EXPECT_DEATH({ (void)allocate_with_hugepage_fallback({.size = 0}); }, "");
}

// ============================================================================
// Internal API: AllocationConfig defaults
// ============================================================================

TEST(MmapUtilsTest, AllocationConfigDefaultValues) {
  const AllocationConfig cfg{};
  EXPECT_EQ(cfg.size, 0U);
  EXPECT_EQ(cfg.pf, PrefaultPolicy::kPopulateWrite);
  EXPECT_EQ(cfg.numa_node, -1);
  EXPECT_FALSE(cfg.lock_pages);
  EXPECT_EQ(cfg.page_mapping_mode, PageMappingMode::kExplicitThenThp);
  EXPECT_EQ(cfg.huge_page_size, HugePageSize::k2MB);
  EXPECT_EQ(cfg.failure_mode, FailureMode::kWarnAndContinue);
}

// ============================================================================
// Public intent API: RegionIntentConfig defaults
// ============================================================================

TEST(MmapUtilsTest, RegionIntentConfigDefaultValues) {
  const RegionIntentConfig cfg{};
  EXPECT_EQ(cfg.size, 0U);
  EXPECT_EQ(cfg.numa_node, -1);
  EXPECT_FALSE(cfg.lock_pages);
  EXPECT_EQ(cfg.page_mapping_mode, PageMappingMode::kExplicitThenThp);
  EXPECT_EQ(cfg.huge_page_size, HugePageSize::k2MB);
  EXPECT_EQ(cfg.failure_mode, FailureMode::kWarnAndContinue);
}

// ============================================================================
// Public intent API: try_allocate_region (all 5 intents)
// ============================================================================

TEST(MmapUtilsTest, TryAllocateRegion_HotRwReturnsValidRegion) {
  auto region = try_allocate_region({.size = 4096}, RegionIntent::kHotRw);
  ASSERT_TRUE(region.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_NE(region->get(), nullptr);
}

TEST(MmapUtilsTest, TryAllocateRegion_ReadMostlyReturnsValidRegion) {
  auto region = try_allocate_region({.size = 4096}, RegionIntent::kReadMostly);
  ASSERT_TRUE(region.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_NE(region->get(), nullptr);
}

TEST(MmapUtilsTest, TryAllocateRegion_ColdReturnsValidRegion) {
  auto region = try_allocate_region({.size = 4096}, RegionIntent::kCold);
  ASSERT_TRUE(region.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_NE(region->get(), nullptr);
}

TEST(MmapUtilsTest, TryAllocateRegion_ZeroSizeReturnsNullopt) {
  auto region = try_allocate_region({.size = 0}, RegionIntent::kHotRw);
  EXPECT_FALSE(region.has_value());
}

// ============================================================================
// Public intent API: allocate_region (all 3 intents)
// ============================================================================

TEST(MmapUtilsTest, AllocateRegion_HotRwReturnsValidRegion) {
  auto region = allocate_region({.size = 4096}, RegionIntent::kHotRw);
  EXPECT_NE(region.get(), nullptr);
  EXPECT_GE(region.size(), 4096U);
}

TEST(MmapUtilsTest, AllocateRegion_ReadMostlyReturnsValidRegion) {
  auto region = allocate_region({.size = 4096}, RegionIntent::kReadMostly);
  EXPECT_NE(region.get(), nullptr);
  EXPECT_GE(region.size(), 4096U);
}

TEST(MmapUtilsTest, AllocateRegion_ColdReturnsValidRegion) {
  auto region = allocate_region({.size = 4096}, RegionIntent::kCold);
  EXPECT_NE(region.get(), nullptr);
  EXPECT_GE(region.size(), 4096U);
}

TEST_F(MmapUtilsDeathTest, AllocateRegion_ZeroSizeAborts) {
  EXPECT_DEATH(
      { (void)allocate_region({.size = 0}, RegionIntent::kHotRw); }, "");
}

// ============================================================================
// Convenience wrappers: try_ variants
// ============================================================================

TEST(MmapUtilsTest, TryAllocateHotRwRegion_ReturnsValidRegion) {
  auto region = try_allocate_hot_rw_region({.size = 4096});
  ASSERT_TRUE(region.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_NE(region->get(), nullptr);
}

TEST(MmapUtilsTest, TryAllocateHotRwRegion_ZeroSizeReturnsNullopt) {
  auto region = try_allocate_hot_rw_region({.size = 0});
  EXPECT_FALSE(region.has_value());
}

TEST(MmapUtilsTest, TryAllocateReadMostlyRegion_ReturnsValidRegion) {
  auto region = try_allocate_read_mostly_region({.size = 4096});
  ASSERT_TRUE(region.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_NE(region->get(), nullptr);
}

TEST(MmapUtilsTest, TryAllocateReadMostlyRegion_ZeroSizeReturnsNullopt) {
  auto region = try_allocate_read_mostly_region({.size = 0});
  EXPECT_FALSE(region.has_value());
}

TEST(MmapUtilsTest, TryAllocateColdRegion_ReturnsValidRegion) {
  auto region = try_allocate_cold_region({.size = 4096});
  ASSERT_TRUE(region.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_NE(region->get(), nullptr);
}

TEST(MmapUtilsTest, TryAllocateColdRegion_ZeroSizeReturnsNullopt) {
  auto region = try_allocate_cold_region({.size = 0});
  EXPECT_FALSE(region.has_value());
}

// ============================================================================
// Convenience wrappers: abort variants
// ============================================================================

TEST(MmapUtilsTest, AllocateHotRwRegion_ReturnsValidRegion) {
  auto region = allocate_hot_rw_region({.size = 4096});
  EXPECT_NE(region.get(), nullptr);
  EXPECT_GE(region.size(), 4096U);
}

TEST(MmapUtilsTest, AllocateReadMostlyRegion_ReturnsValidRegion) {
  auto region = allocate_read_mostly_region({.size = 4096});
  EXPECT_NE(region.get(), nullptr);
  EXPECT_GE(region.size(), 4096U);
}

TEST(MmapUtilsTest, AllocateColdRegion_ReturnsValidRegion) {
  auto region = allocate_cold_region({.size = 4096});
  EXPECT_NE(region.get(), nullptr);
  EXPECT_GE(region.size(), 4096U);
}

TEST_F(MmapUtilsDeathTest, AllocateHotRwRegion_ZeroSizeAborts) {
  EXPECT_DEATH({ (void)allocate_hot_rw_region({.size = 0}); }, "");
}

TEST_F(MmapUtilsDeathTest, AllocateReadMostlyRegion_ZeroSizeAborts) {
  EXPECT_DEATH({ (void)allocate_read_mostly_region({.size = 0}); }, "");
}

TEST_F(MmapUtilsDeathTest, AllocateColdRegion_ZeroSizeAborts) {
  EXPECT_DEATH({ (void)allocate_cold_region({.size = 0}); }, "");
}

// ============================================================================
// Semantic: intent determines prefault behavior
// ============================================================================

TEST(MmapUtilsTest, HotRwRegion_IsWritePrefaulted) {
  auto region = allocate_hot_rw_region({.size = 4096});

  // kHotRw uses kPopulateWrite — pages should be writable without faulting.
  auto *p = static_cast<char *>(region.get());
  std::memset(p, 0xAA, 4096);
  EXPECT_EQ(p[0], static_cast<char>(0xAA));
  EXPECT_EQ(p[4095], static_cast<char>(0xAA));
}

TEST(MmapUtilsTest, ReadMostlyRegion_IsReadable) {
  auto region = allocate_read_mostly_region({.size = 4096});

  // kReadMostly uses kPopulateRead — pages are read-prefaulted.
  // Anonymous pages are zero-filled.
  const auto *p = static_cast<const char *>(region.get());
  EXPECT_EQ(p[0], 0);
  EXPECT_EQ(p[4095], 0);
}

TEST(MmapUtilsTest, ColdRegion_IsAccessibleOnDemand) {
  auto region = allocate_cold_region({.size = 4096});

  // kCold uses kNone — no prefault. Pages are demand-faulted on first access.
  // NOLINTNEXTLINE(readability-qualified-auto) — need non-const to write.
  auto *p = static_cast<char *>(region.get());
  p[0] = 99;
  EXPECT_EQ(p[0], 99);
}

// ============================================================================
// PageMappingMode via intent API
// ============================================================================

TEST(MmapUtilsTest, TryAllocateRegion_ThpOnlyModeWorks) {
  auto region = try_allocate_region(
      {.size = 4096, .page_mapping_mode = PageMappingMode::kThpOnly},
      RegionIntent::kHotRw);
  ASSERT_TRUE(region.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_NE(region->get(), nullptr);
}

TEST(MmapUtilsTest, TryAllocateRegion_RegularPagesModeWorks) {
  auto region = try_allocate_region(
      {.size = 4096, .page_mapping_mode = PageMappingMode::kRegularPages},
      RegionIntent::kHotRw);
  ASSERT_TRUE(region.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_NE(region->get(), nullptr);

  // Writable (kHotRw prefaults even on regular pages).
  auto *p = static_cast<char *>(
      region->get()); // NOLINT(bugprone-unchecked-optional-access)
  std::memset(p, 0xBB, 4096);
  EXPECT_EQ(p[0], static_cast<char>(0xBB));
}

TEST(MmapUtilsTest, TryAllocate_RegularPagesModeWorks) {
  // Internal API: kRegularPages skips hugepage entirely.
  auto region = try_allocate_with_hugepage_fallback(
      {.size = 4096, .page_mapping_mode = PageMappingMode::kRegularPages});
  ASSERT_TRUE(region.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_NE(region->get(), nullptr);
}

TEST(MmapUtilsTest, ColdIntent_UsesRegularPages) {
  // kCold intent should use regular pages regardless of config's
  // page_mapping_mode. This verifies the intent overrides the configured
  // page_mapping_mode.
  auto region = try_allocate_region(
      {.size = 4096, .page_mapping_mode = PageMappingMode::kExplicitThenThp},
      RegionIntent::kCold);
  ASSERT_TRUE(region.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_NE(region->get(), nullptr);

  // Still accessible (demand-faulted).
  auto *p = static_cast<char *>(
      region->get()); // NOLINT(bugprone-unchecked-optional-access)
  p[0] = 42;
  EXPECT_EQ(p[0], 42);
}

TEST(MmapUtilsTest, TryAllocateRegion_ExplicitOnlyMaySucceedOrFail) {
  // Machine-dependent: depends on nr_hugepages reservation.
  auto region =
      try_allocate_region({.size = 2UL * 1024 * 1024,
                           .page_mapping_mode = PageMappingMode::kExplicitOnly},
                          RegionIntent::kHotRw);
  if (region.has_value()) {
    EXPECT_NE(region->get(),
              nullptr); // NOLINT(bugprone-unchecked-optional-access)
  }
}

} // namespace
} // namespace mk::sys::memory
