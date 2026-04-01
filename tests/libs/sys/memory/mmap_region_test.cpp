/**
 * @file mmap_region_test.cpp
 * @brief GTest-based tests for MmapRegion (RAII mmap wrapper).
 *
 * Test plan:
 *   RAII & Move Semantics:
 *     1. Default constructs invalid
 *     2. Destructor tolerates invalid region
 *     3. Move construct transfers ownership
 *     4. Move assign transfers and unmaps previous
 *     5. Self move assign is no-op
 *     6. Not copyable / is movable (static_assert)
 *
 *   Factory: Anonymous:
 *     7. Allocate returns valid region
 *     8. Size rounded up to page boundary
 *     9. Zero size returns nullopt
 *    10. Large allocation works (2MB)
 *
 *   Factory: Huge Pages (GTEST_SKIP if unavailable):
 *    11. Allocate succeeds when available
 *    12. Size rounded up to huge page boundary
 *    12b. Allocate 2GB pool (1024 × 2MB pages, prefaulted, stride-verified)
 *
 *   Factory: Shared Memory:
 *    13. Create and open round trip
 *    14. Open nonexistent fails
 *    14b. Open existing with oversized request fails (fstat validation)
 *
 *   Factory: File-backed:
 *    15. Map read-only file
 *    16. Map writable file
 *    16b. Non-aligned offset returns nullopt
 *    16c. Map beyond EOF returns nullopt (fstat validation)
 *    16d. Map with offset beyond EOF returns nullopt
 *
 *   Observers & Mutators:
 *    17. Release transfers ownership
 *    18. Release on invalid returns MAP_FAILED
 *    19. Reset unmaps and invalidates
 *    20. Reset on invalid is no-op
 *    21. Swap exchanges regions
 *    22. ADL swap works
 *
 *   protect() Death Tests:
 *    22b. Read-only prevents write (SIGSEGV)
 *    22c. PROT_NONE prevents access (SIGSEGV)
 *
 *   Memory Operations:
 *    23. Prefault write touch works
 *    24. Prefault on invalid returns false
 *    24b. Prefault read-touch preserves existing data (shared memory)
 *    25. Prefault policy populate write (in factory)
 *    26. Prefault policy manual write (in factory)
 *    26b. Prefault policy populate read (in factory)
 *    26c. Prefault policy manual read (in factory)
 *    26d. Prefault read policy preserves shared memory data
 *    26e. Prefault manual read policy preserves shared memory data
 *    27. Advise huge page hint
 *    28. Lock and unlock (GTEST_SKIP if RLIMIT_MEMLOCK insufficient)
 */

#include "sys/memory/mmap_region.hpp"

#include "sys/hardware_constants.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

using mk::sys::memory::HugePageSize;
using mk::sys::memory::MmapRegion;
using mk::sys::memory::PrefaultPolicy;
using mk::sys::memory::ShmMode;

// ============================================================================
// RAII & Move Semantics
// ============================================================================

TEST(MmapRegionTest, DefaultConstructsInvalid) {
  const MmapRegion region;
  EXPECT_EQ(nullptr, region.get());
  EXPECT_EQ(nullptr, region.data());
  EXPECT_EQ(0U, region.size());
  EXPECT_FALSE(region.is_valid());
  EXPECT_FALSE(static_cast<bool>(region));
}

TEST(MmapRegionTest, DestructorToleratesInvalidRegion) {
  // Default-constructed region destroyed without crash.
  {
    const MmapRegion region;
  }
  SUCCEED();
}

TEST(MmapRegionTest, MoveConstructTransfersOwnership) {
  auto opt = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(opt.has_value());

  void *original_addr =
      opt->get(); // NOLINT(bugprone-unchecked-optional-access)
  const std::size_t original_size =
      opt->size(); // NOLINT(bugprone-unchecked-optional-access)

  const MmapRegion moved(
      std::move(*opt)); // NOLINT(bugprone-unchecked-optional-access)

  // Source is invalid after move.
  EXPECT_FALSE(opt->is_valid()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(0U, opt->size());    // NOLINT(bugprone-unchecked-optional-access)

  // Destination has original values.
  EXPECT_EQ(original_addr, moved.get());
  EXPECT_EQ(original_size, moved.size());
  EXPECT_TRUE(moved.is_valid());
}

TEST(MmapRegionTest, MoveAssignTransfersAndUnmapsPrevious) {
  auto opt_a = MmapRegion::allocate_anonymous(4096);
  auto opt_b = MmapRegion::allocate_anonymous(8192);
  ASSERT_TRUE(opt_a.has_value());
  ASSERT_TRUE(opt_b.has_value());

  void *addr_b = opt_b->get(); // NOLINT(bugprone-unchecked-optional-access)
  const std::size_t size_b =
      opt_b->size(); // NOLINT(bugprone-unchecked-optional-access)

  // Move-assign b into a. Previous mapping of a is unmapped.
  *opt_a = std::move(*opt_b); // NOLINT(bugprone-unchecked-optional-access)

  EXPECT_EQ(addr_b, opt_a->get()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(size_b,
            opt_a->size());        // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_FALSE(opt_b->is_valid()); // NOLINT(bugprone-unchecked-optional-access)
}

TEST(MmapRegionTest, SelfMoveAssignIsNoop) {
  auto opt = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(opt.has_value());

  void *addr = opt->get(); // NOLINT(bugprone-unchecked-optional-access)
  const std::size_t size =
      opt->size(); // NOLINT(bugprone-unchecked-optional-access)

  // Self move-assign should be a no-op.
  auto &ref = *opt; // NOLINT(bugprone-unchecked-optional-access)
  ref = std::move(ref);

  EXPECT_EQ(addr, opt->get());  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(size, opt->size()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_TRUE(opt->is_valid()); // NOLINT(bugprone-unchecked-optional-access)
}

TEST(MmapRegionTest, NotCopyableIsMovable) {
  static_assert(!std::is_copy_constructible_v<MmapRegion>);
  static_assert(!std::is_copy_assignable_v<MmapRegion>);
  static_assert(std::is_move_constructible_v<MmapRegion>);
  static_assert(std::is_move_assignable_v<MmapRegion>);
}

// ============================================================================
// Factory: Anonymous
// ============================================================================

TEST(MmapRegionAnonymous, AllocateReturnsValidRegion) {
  auto opt = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(opt.has_value());
  EXPECT_NE(nullptr, opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_GE(opt->size(), 4096U);  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_TRUE(opt->is_valid());   // NOLINT(bugprone-unchecked-optional-access)

  // Write and read back to verify the memory is usable.
  auto *ptr = static_cast<std::uint64_t *>(
      opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  *ptr = 0xDEADBEEFCAFEBABE;
  EXPECT_EQ(0xDEADBEEFCAFEBABE, *ptr);
}

TEST(MmapRegionAnonymous, SizeRoundedUpToPageBoundary) {
  auto opt1 = MmapRegion::allocate_anonymous(1);
  ASSERT_TRUE(opt1.has_value());
  EXPECT_EQ(4096U, opt1->size()); // NOLINT(bugprone-unchecked-optional-access)

  auto opt2 = MmapRegion::allocate_anonymous(4097);
  ASSERT_TRUE(opt2.has_value());
  EXPECT_EQ(8192U, opt2->size()); // NOLINT(bugprone-unchecked-optional-access)
}

TEST(MmapRegionAnonymous, ZeroSizeReturnsNullopt) {
  auto opt = MmapRegion::allocate_anonymous(0);
  EXPECT_FALSE(opt.has_value());
}

TEST(MmapRegionAnonymous, LargeAllocationWorks) {
  auto opt = MmapRegion::allocate_anonymous(mk::sys::kHugePageSize2MB);
  ASSERT_TRUE(opt.has_value());
  EXPECT_EQ(mk::sys::kHugePageSize2MB,
            opt->size()); // NOLINT(bugprone-unchecked-optional-access)

  // Write to first and last page to verify the entire region is usable.
  auto *data = opt->data(); // NOLINT(bugprone-unchecked-optional-access)
  data[0] = std::byte{0x42};
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  data[opt->size() - 1] = std::byte{0x43};
  EXPECT_EQ(std::byte{0x42}, data[0]);
  EXPECT_EQ(
      std::byte{0x43},
      data[opt->size() - 1]); // NOLINT(bugprone-unchecked-optional-access)
}

// ============================================================================
// Factory: Huge Pages (GTEST_SKIP if unavailable)
// ============================================================================

TEST(MmapRegionHugePages, AllocateSucceedsWhenAvailable) {
  auto opt = MmapRegion::allocate_huge_pages(mk::sys::kHugePageSize2MB);
  if (!opt.has_value()) {
    GTEST_SKIP() << "Huge pages not available on this system "
                    "(check /proc/sys/vm/nr_hugepages)";
  }

  EXPECT_EQ(mk::sys::kHugePageSize2MB,
            opt->size());         // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_NE(nullptr, opt->get()); // NOLINT(bugprone-unchecked-optional-access)

  // Verify writable.
  auto *ptr = static_cast<std::uint64_t *>(
      opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  *ptr = 0x1234567890ABCDEF;
  EXPECT_EQ(0x1234567890ABCDEF, *ptr);
}

TEST(MmapRegionHugePages, SizeRoundedUpToHugePageBoundary) {
  auto opt = MmapRegion::allocate_huge_pages(1, HugePageSize::k2MB);
  if (!opt.has_value()) {
    GTEST_SKIP() << "Huge pages not available on this system";
  }

  EXPECT_EQ(mk::sys::kHugePageSize2MB,
            opt->size()); // NOLINT(bugprone-unchecked-optional-access)
}

TEST(MmapRegionHugePages, Allocate2GBPool) {
  // Allocate 2GB in a single MAP_HUGETLB mapping — 1024 × 2MB huge pages.
  // Requires: vm.nr_hugepages >= 1024, RLIMIT_MEMLOCK >= 2GB (or unlimited).
  constexpr std::size_t k2Gb = static_cast<std::size_t>(2) * 1024 * 1024 * 1024;

  auto opt = MmapRegion::allocate_huge_pages(k2Gb, HugePageSize::k2MB,
                                             PrefaultPolicy::kPopulateWrite);
  if (!opt.has_value()) {
    GTEST_SKIP()
        << "2GB huge page allocation failed — need nr_hugepages >= 1024 "
           "and RLIMIT_MEMLOCK >= 2GB. See studies/caos/docs/"
           "linux_memory_config.md for setup instructions.";
  }

  EXPECT_EQ(k2Gb, opt->size());   // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_NE(nullptr, opt->get()); // NOLINT(bugprone-unchecked-optional-access)

  auto *data = static_cast<std::byte *>(
      opt->get()); // NOLINT(bugprone-unchecked-optional-access)

  // Write sentinel at the beginning and end of the region.
  data[0] = std::byte{0xAA};
  data[k2Gb - 1] = std::byte{0xBB};
  EXPECT_EQ(std::byte{0xAA}, data[0]);
  EXPECT_EQ(std::byte{0xBB}, data[k2Gb - 1]);

  // Stride-write across the entire region: touch one byte per 2MB page.
  // Verifies all 1024 huge pages are usable, not just the first and last.
  for (std::size_t offset = 0; offset < k2Gb;
       offset += mk::sys::kHugePageSize2MB) {
    data[offset] = std::byte{0xCC};
  }
  // Spot-check a few stride positions.
  EXPECT_EQ(std::byte{0xCC}, data[0]);
  EXPECT_EQ(std::byte{0xCC}, data[mk::sys::kHugePageSize2MB]);
  EXPECT_EQ(std::byte{0xCC}, data[mk::sys::kHugePageSize2MB * 512]);
  EXPECT_EQ(std::byte{0xCC}, data[mk::sys::kHugePageSize2MB * 1023]);
}

// ============================================================================
// Factory: Shared Memory
// ============================================================================

class MmapRegionSharedTest : public ::testing::Test {
protected:
  // Use PID in name to avoid collisions with parallel test runs.
  std::string shm_name_;

  void SetUp() override {
    shm_name_ = "/mmap_test_" + std::to_string(::getpid());
  }

  void TearDown() override {
    // Clean up the shared memory object regardless of test outcome.
    ::shm_unlink(shm_name_.c_str());
  }
};

TEST_F(MmapRegionSharedTest, CreateAndOpenRoundTrip) {
  // Producer creates and writes a pattern.
  auto producer =
      MmapRegion::open_shared(shm_name_, 4096, ShmMode::kCreateOrOpen);
  ASSERT_TRUE(producer.has_value())
      << "shm_open failed — check permissions or /dev/shm availability";

  auto *write_ptr = static_cast<std::uint64_t *>(
      producer->get()); // NOLINT(bugprone-unchecked-optional-access)
  constexpr std::uint64_t kMagic = 0xCAFEBABE12345678;
  *write_ptr = kMagic;

  // Consumer opens the same region and reads back.
  auto consumer =
      MmapRegion::open_shared(shm_name_, 4096, ShmMode::kOpenExisting);
  ASSERT_TRUE(consumer.has_value());

  const auto *read_ptr = static_cast<const std::uint64_t *>(
      consumer->get()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(kMagic, *read_ptr);
}

TEST(MmapRegionShared, OpenNonexistentFails) {
  auto opt = MmapRegion::open_shared("/mmap_nonexistent_should_not_exist", 4096,
                                     ShmMode::kOpenExisting);
  EXPECT_FALSE(opt.has_value());
}

TEST_F(MmapRegionSharedTest, OpenExistingOversizedFails) {
  // Producer creates a 4096-byte shared memory object.
  auto producer =
      MmapRegion::open_shared(shm_name_, 4096, ShmMode::kCreateOrOpen);
  ASSERT_TRUE(producer.has_value());

  // Consumer tries to open with a size larger than the actual object.
  // This should fail (fstat detects the mismatch) instead of silently
  // mapping beyond the object's bounds and risking SIGBUS.
  auto consumer = MmapRegion::open_shared(shm_name_, std::size_t{4096} * 10,
                                          ShmMode::kOpenExisting);
  EXPECT_FALSE(consumer.has_value());
}

// ============================================================================
// Factory: File-backed
// ============================================================================

class MmapRegionFileTest : public ::testing::Test {
protected:
  std::string tmp_path_;
  int fd_ = -1;

  void SetUp() override {
    // Create a temporary file with known content.
    tmp_path_ = "/tmp/mmap_region_test_XXXXXX";
    fd_ = ::mkstemp(tmp_path_.data());
    ASSERT_GE(fd_, 0) << "mkstemp failed, errno=" << errno;
  }

  void TearDown() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    ::unlink(tmp_path_.c_str());
  }
};

TEST_F(MmapRegionFileTest, MapReadOnlyFile) {
  // Write known content to the temp file.
  constexpr char kData[] = "Hello, MmapRegion!";
  const std::size_t data_len = sizeof(kData);

  ::ftruncate(fd_, 4096);
  ASSERT_EQ(static_cast<ssize_t>(data_len), ::write(fd_, kData, data_len));

  auto opt = MmapRegion::map_file(fd_, 4096, 0, false);
  ASSERT_TRUE(opt.has_value());
  EXPECT_EQ(4096U, opt->size()); // NOLINT(bugprone-unchecked-optional-access)

  // Verify mapped content matches.
  const auto *mapped = static_cast<const char *>(
      opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(0, std::memcmp(mapped, kData, data_len));
}

TEST_F(MmapRegionFileTest, MapWritableFile) {
  ::ftruncate(fd_, 4096);

  auto opt = MmapRegion::map_file(fd_, 4096, 0, true);
  ASSERT_TRUE(opt.has_value());

  // Write through the mapping.
  constexpr std::uint64_t kMagic = 0xFEEDFACEDEADC0DE;
  auto *ptr = static_cast<std::uint64_t *>(
      opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  *ptr = kMagic;

  // Read back via the mapping.
  EXPECT_EQ(kMagic, *ptr);

  // Read back from the file to verify persistence.
  std::uint64_t file_val = 0;
  ::lseek(fd_, 0, SEEK_SET);
  ASSERT_EQ(static_cast<ssize_t>(sizeof(file_val)),
            ::read(fd_, &file_val, sizeof(file_val)));
  EXPECT_EQ(kMagic, file_val);
}

TEST_F(MmapRegionFileTest, NonAlignedOffsetReturnsNullopt) {
  ::ftruncate(fd_, 8192);

  // Offset 100 is not page-aligned — should fail immediately.
  auto opt = MmapRegion::map_file(fd_, 4096, 100, false);
  EXPECT_FALSE(opt.has_value());
}

TEST_F(MmapRegionFileTest, MapBeyondEofReturnsNullopt) {
  // File is 4096 bytes, but we request 8192 — should fail with fstat check.
  ::ftruncate(fd_, 4096);

  auto opt = MmapRegion::map_file(fd_, 8192, 0, false);
  EXPECT_FALSE(opt.has_value());
}

TEST_F(MmapRegionFileTest, MapWithOffsetBeyondEofReturnsNullopt) {
  // File is 8192 bytes. Mapping 4096 at offset 8192 exceeds the file.
  ::ftruncate(fd_, 8192);

  auto opt = MmapRegion::map_file(fd_, 4096, 8192, false);
  EXPECT_FALSE(opt.has_value());
}

// ============================================================================
// Observers & Mutators
// ============================================================================

TEST(MmapRegionTest, ReleaseTransfersOwnership) {
  auto opt = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(opt.has_value());

  void *expected_addr =
      opt->get(); // NOLINT(bugprone-unchecked-optional-access)
  const std::size_t expected_size =
      opt->size(); // NOLINT(bugprone-unchecked-optional-access)

  auto raw = opt->release(); // NOLINT(bugprone-unchecked-optional-access)

  // Region is now invalid.
  EXPECT_FALSE(opt->is_valid()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(0U, opt->size());    // NOLINT(bugprone-unchecked-optional-access)

  // Returned RawRegion has the original values.
  EXPECT_EQ(expected_addr, raw.addr);
  EXPECT_EQ(expected_size, raw.size);

  // Manual cleanup — caller is responsible after release().
  ::munmap(raw.addr, raw.size);
}

TEST(MmapRegionTest, ReleaseOnInvalidReturnsMapFailed) {
  MmapRegion region;
  auto raw = region.release();
  // release() returns the raw internal sentinel (MAP_FAILED), not nullptr.
  // Callers must check addr != MAP_FAILED before calling munmap.
  EXPECT_EQ(MAP_FAILED, raw.addr);
  EXPECT_EQ(0U, raw.size);
}

TEST(MmapRegionTest, ResetUnmapsAndInvalidates) {
  auto opt = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(opt.has_value());
  EXPECT_TRUE(opt->is_valid()); // NOLINT(bugprone-unchecked-optional-access)

  opt->reset(); // NOLINT(bugprone-unchecked-optional-access)

  EXPECT_FALSE(opt->is_valid());  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(nullptr, opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(0U, opt->size());     // NOLINT(bugprone-unchecked-optional-access)
}

TEST(MmapRegionTest, ResetOnInvalidIsNoop) {
  MmapRegion region;
  region.reset(); // Should not crash.
  EXPECT_FALSE(region.is_valid());
}

TEST(MmapRegionTest, SwapExchangesRegions) {
  auto opt_a = MmapRegion::allocate_anonymous(4096);
  auto opt_b = MmapRegion::allocate_anonymous(8192);
  ASSERT_TRUE(opt_a.has_value());
  ASSERT_TRUE(opt_b.has_value());

  void *addr_a = opt_a->get(); // NOLINT(bugprone-unchecked-optional-access)
  const std::size_t size_a =
      opt_a->size();           // NOLINT(bugprone-unchecked-optional-access)
  void *addr_b = opt_b->get(); // NOLINT(bugprone-unchecked-optional-access)
  const std::size_t size_b =
      opt_b->size(); // NOLINT(bugprone-unchecked-optional-access)

  opt_a->swap(*opt_b); // NOLINT(bugprone-unchecked-optional-access)

  EXPECT_EQ(addr_b, opt_a->get()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(size_b,
            opt_a->size());        // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(addr_a, opt_b->get()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(size_a,
            opt_b->size()); // NOLINT(bugprone-unchecked-optional-access)
}

TEST(MmapRegionTest, AdlSwapWorks) {
  auto opt_a = MmapRegion::allocate_anonymous(4096);
  auto opt_b = MmapRegion::allocate_anonymous(8192);
  ASSERT_TRUE(opt_a.has_value());
  ASSERT_TRUE(opt_b.has_value());

  void *addr_a = opt_a->get(); // NOLINT(bugprone-unchecked-optional-access)
  void *addr_b = opt_b->get(); // NOLINT(bugprone-unchecked-optional-access)

  // Unqualified swap should find the ADL friend function.
  using std::swap;
  swap(*opt_a, *opt_b); // NOLINT(bugprone-unchecked-optional-access)

  EXPECT_EQ(addr_b, opt_a->get()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(addr_a, opt_b->get()); // NOLINT(bugprone-unchecked-optional-access)
}

// ============================================================================
// Memory Operations
// ============================================================================

TEST(MmapRegionTest, PrefaultWriteTouchWorks) {
  auto opt = MmapRegion::allocate_anonymous(std::size_t{4096} * 4);
  ASSERT_TRUE(opt.has_value());

  EXPECT_TRUE(
      opt->prefault(true)); // NOLINT(bugprone-unchecked-optional-access)

  // Verify pages are usable (no fault on access).
  auto *data = opt->data(); // NOLINT(bugprone-unchecked-optional-access)
  data[0] = std::byte{0x11};
  data[4096] = std::byte{0x22};
  data[std::size_t{4096} * 2] = std::byte{0x33};
  EXPECT_EQ(std::byte{0x11}, data[0]);
  EXPECT_EQ(std::byte{0x22}, data[4096]);
  EXPECT_EQ(std::byte{0x33}, data[std::size_t{4096} * 2]);
}

TEST(MmapRegionTest, PrefaultOnInvalidReturnsFalse) {
  MmapRegion region;
  EXPECT_FALSE(region.prefault());
}

TEST_F(MmapRegionSharedTest, PrefaultReadTouchPreservesData) {
  // Producer creates shared memory and writes a pattern.
  auto producer =
      MmapRegion::open_shared(shm_name_, 4096, ShmMode::kCreateOrOpen);
  ASSERT_TRUE(producer.has_value());

  auto *write_ptr = static_cast<std::uint64_t *>(
      producer->get()); // NOLINT(bugprone-unchecked-optional-access)
  constexpr std::uint64_t kPattern = 0xABCD1234DEADBEEF;
  *write_ptr = kPattern;

  // Consumer opens the same region and read-prefaults it.
  // Read-touch (prefault(false)) should map pages without overwriting data.
  auto consumer =
      MmapRegion::open_shared(shm_name_, 4096, ShmMode::kOpenExisting);
  ASSERT_TRUE(consumer.has_value());
  EXPECT_TRUE(
      consumer->prefault(false)); // NOLINT(bugprone-unchecked-optional-access)

  const auto *read_ptr = static_cast<const std::uint64_t *>(
      consumer->get()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(kPattern, *read_ptr);
}

TEST(MmapRegionAnonymous, PrefaultPolicyPopulateWrite) {
  auto opt =
      MmapRegion::allocate_anonymous(4096, PrefaultPolicy::kPopulateWrite);
  ASSERT_TRUE(opt.has_value());

  // Verify region is valid and usable.
  auto *ptr = static_cast<std::uint64_t *>(
      opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  *ptr = 42;
  EXPECT_EQ(42U, *ptr);
}

TEST(MmapRegionAnonymous, PrefaultPolicyManualWrite) {
  auto opt = MmapRegion::allocate_anonymous(4096, PrefaultPolicy::kManualWrite);
  ASSERT_TRUE(opt.has_value());

  auto *ptr = static_cast<std::uint64_t *>(
      opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  *ptr = 99;
  EXPECT_EQ(99U, *ptr);
}

TEST(MmapRegionAnonymous, PrefaultPolicyPopulateRead) {
  auto opt =
      MmapRegion::allocate_anonymous(4096, PrefaultPolicy::kPopulateRead);
  ASSERT_TRUE(opt.has_value());

  // Read-prefaulted region should be usable.
  auto *ptr = static_cast<std::uint64_t *>(
      opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  *ptr = 55;
  EXPECT_EQ(55U, *ptr);
}

TEST(MmapRegionAnonymous, PrefaultPolicyManualRead) {
  auto opt = MmapRegion::allocate_anonymous(4096, PrefaultPolicy::kManualRead);
  ASSERT_TRUE(opt.has_value());

  auto *ptr = static_cast<std::uint64_t *>(
      opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  *ptr = 77;
  EXPECT_EQ(77U, *ptr);
}

TEST_F(MmapRegionSharedTest, PrefaultReadPolicyPreservesData) {
  // Producer creates shared memory and writes a pattern.
  auto producer =
      MmapRegion::open_shared(shm_name_, 4096, ShmMode::kCreateOrOpen);
  ASSERT_TRUE(producer.has_value());

  auto *write_ptr = static_cast<std::uint64_t *>(
      producer->get()); // NOLINT(bugprone-unchecked-optional-access)
  constexpr std::uint64_t kPattern = 0x1122334455667788;
  *write_ptr = kPattern;

  // Consumer opens and prefaults with kPopulateRead via factory.
  // The read-prefault should NOT overwrite the producer's data.
  auto consumer = MmapRegion::open_shared(
      shm_name_, 4096, ShmMode::kOpenExisting, PrefaultPolicy::kPopulateRead);
  ASSERT_TRUE(consumer.has_value());

  const auto *read_ptr = static_cast<const std::uint64_t *>(
      consumer->get()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(kPattern, *read_ptr);
}

TEST_F(MmapRegionSharedTest, PrefaultManualReadPolicyPreservesData) {
  // Producer creates shared memory and writes a pattern.
  auto producer =
      MmapRegion::open_shared(shm_name_, 4096, ShmMode::kCreateOrOpen);
  ASSERT_TRUE(producer.has_value());

  auto *write_ptr = static_cast<std::uint64_t *>(
      producer->get()); // NOLINT(bugprone-unchecked-optional-access)
  constexpr std::uint64_t kPattern = 0xAABBCCDDEEFF0011;
  *write_ptr = kPattern;

  // Consumer opens and prefaults with kManualRead via factory.
  auto consumer = MmapRegion::open_shared(
      shm_name_, 4096, ShmMode::kOpenExisting, PrefaultPolicy::kManualRead);
  ASSERT_TRUE(consumer.has_value());

  const auto *read_ptr = static_cast<const std::uint64_t *>(
      consumer->get()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(kPattern, *read_ptr);
}

TEST(MmapRegionTest, AdviseHugePageHint) {
  // Need at least 2MB for MADV_HUGEPAGE to be meaningful.
  auto opt = MmapRegion::allocate_anonymous(mk::sys::kHugePageSize2MB);
  ASSERT_TRUE(opt.has_value());

  // MADV_HUGEPAGE may fail if THP is disabled — not a test failure.
  const bool result =
      opt->advise(MADV_HUGEPAGE); // NOLINT(bugprone-unchecked-optional-access)
  // We just verify the method doesn't crash; the return value depends
  // on kernel configuration.
  (void)result;
  SUCCEED();
}

TEST(MmapRegionTest, LockAndUnlock) {
  auto opt = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(opt.has_value());

  const bool locked = opt->lock(); // NOLINT(bugprone-unchecked-optional-access)
  if (!locked) {
    GTEST_SKIP() << "mlock failed — insufficient RLIMIT_MEMLOCK "
                    "(common in containers)";
  }

  EXPECT_TRUE(opt->unlock()); // NOLINT(bugprone-unchecked-optional-access)
}

TEST(MmapRegionTest, LockOnInvalidReturnsFalse) {
  MmapRegion region;
  EXPECT_FALSE(region.lock());
  EXPECT_FALSE(region.unlock());
}

TEST(MmapRegionTest, AdviseOnInvalidReturnsFalse) {
  MmapRegion region;
  EXPECT_FALSE(region.advise(MADV_HUGEPAGE));
}

// ============================================================================
// protect() — mprotect wrapper
// ============================================================================

TEST(MmapRegionTest, ProtectReadOnly) {
  auto opt = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(opt.has_value());

  // Write before protecting.
  auto *ptr = static_cast<std::uint64_t *>(
      opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  *ptr = 42;
  EXPECT_EQ(42U, *ptr);

  // Mark read-only.
  EXPECT_TRUE(
      opt->protect(PROT_READ)); // NOLINT(bugprone-unchecked-optional-access)

  // Read should still work.
  EXPECT_EQ(42U, *ptr);

  // Restore to read-write for clean destruction.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_TRUE(opt->protect(PROT_READ | PROT_WRITE));
}

TEST(MmapRegionTest, ProtectNoneCreatesGuardPage) {
  auto opt = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(opt.has_value());

  // Mark as guard page (no access).
  EXPECT_TRUE(
      opt->protect(PROT_NONE)); // NOLINT(bugprone-unchecked-optional-access)

  // Restore for clean destruction.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_TRUE(opt->protect(PROT_READ | PROT_WRITE));
}

TEST(MmapRegionTest, ProtectOnInvalidReturnsFalse) {
  MmapRegion region;
  EXPECT_FALSE(region.protect(PROT_READ));
}

// Death tests: verify memory protection actually prevents illegal access.
// EXPECT_DEATH forks a child process, so the parent's mapping is unaffected.

TEST(MmapRegionDeathTest, ProtectReadOnlyPreventsWrite) {
  auto opt = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(opt.has_value());

  auto *ptr = static_cast<int *>(
      opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_TRUE(
      opt->protect(PROT_READ)); // NOLINT(bugprone-unchecked-optional-access)

  // Writing to read-only memory should cause SIGSEGV.
  EXPECT_DEATH(*ptr = 1, ".*");
}

TEST(MmapRegionDeathTest, ProtectNonePreventsAccess) {
  auto opt = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(opt.has_value());

  auto *ptr = static_cast<volatile int *>(
      opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_TRUE(
      opt->protect(PROT_NONE)); // NOLINT(bugprone-unchecked-optional-access)

  // Any access to a PROT_NONE page should cause SIGSEGV.
  EXPECT_DEATH({ [[maybe_unused]] const int x = *ptr; }, ".*");
}

// ============================================================================
// remap() — mremap wrapper
// ============================================================================

TEST(MmapRegionTest, RemapGrowsMapping) {
  auto opt = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(opt.has_value());

  // Write a sentinel before grow.
  auto *ptr = static_cast<std::uint64_t *>(
      opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  *ptr = 0xDEADBEEF;

  // Grow to 2 pages.
  EXPECT_TRUE(opt->remap(8192)); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(8192U, opt->size()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_TRUE(opt->is_valid());  // NOLINT(bugprone-unchecked-optional-access)

  // Original data should be preserved (mremap copies).
  ptr = static_cast<std::uint64_t *>(
      opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(0xDEADBEEFU, *ptr);

  // New space should be usable.
  auto *data = opt->data(); // NOLINT(bugprone-unchecked-optional-access)
  data[4096] = std::byte{0x42};
  EXPECT_EQ(std::byte{0x42}, data[4096]);
}

TEST(MmapRegionTest, RemapShrinksMapping) {
  auto opt = MmapRegion::allocate_anonymous(8192);
  ASSERT_TRUE(opt.has_value());

  EXPECT_TRUE(opt->remap(4096)); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(4096U, opt->size()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_TRUE(opt->is_valid());  // NOLINT(bugprone-unchecked-optional-access)
}

TEST(MmapRegionTest, RemapOnInvalidReturnsFalse) {
  MmapRegion region;
  EXPECT_FALSE(region.remap(4096));
}

TEST(MmapRegionTest, RemapZeroSizeReturnsFalse) {
  auto opt = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(opt.has_value());
  EXPECT_FALSE(opt->remap(0)); // NOLINT(bugprone-unchecked-optional-access)
  // Region should be unchanged.
  EXPECT_EQ(4096U, opt->size()); // NOLINT(bugprone-unchecked-optional-access)
}

// ============================================================================
// bind_numa_node() — NUMA placement
// ============================================================================

TEST(MmapRegionTest, BindNumaNodeOnInvalidReturnsFalse) {
  MmapRegion region;
  EXPECT_FALSE(region.bind_numa_node(0));
}

TEST(MmapRegionTest, BindNumaNodeNegativeReturnsFalse) {
  auto opt = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(opt.has_value());
  EXPECT_FALSE(
      opt->bind_numa_node(-1)); // NOLINT(bugprone-unchecked-optional-access)
}

TEST(MmapRegionTest, BindNumaNodeOutOfRangeReturnsFalse) {
  auto opt = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(opt.has_value());
  // Implementation limits node IDs to [0, 64) — single unsigned long bitmask.
  EXPECT_FALSE(
      opt->bind_numa_node(64)); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_FALSE(
      opt->bind_numa_node(65)); // NOLINT(bugprone-unchecked-optional-access)
}

TEST(MmapRegionTest, BindNumaNode0Succeeds) {
  auto opt = MmapRegion::allocate_anonymous(4096);
  ASSERT_TRUE(opt.has_value());

  // Node 0 should exist on all systems with NUMA support.
  const bool result =
      opt->bind_numa_node(0); // NOLINT(bugprone-unchecked-optional-access)
  if (!result) {
    GTEST_SKIP() << "mbind failed — system may not have NUMA support "
                    "(single-socket or CONFIG_NUMA=n)";
  }

  // Verify region is still usable after binding.
  auto *ptr = static_cast<std::uint64_t *>(
      opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  *ptr = 123;
  EXPECT_EQ(123U, *ptr);
}

// ============================================================================
// MAP_LOCKED — lock pages at creation time
// ============================================================================

TEST(MmapRegionTest, AllocateAnonymousWithMapLocked) {
  auto opt = MmapRegion::allocate_anonymous(4096, PrefaultPolicy::kNone, true);
  if (!opt.has_value()) {
    GTEST_SKIP() << "MAP_LOCKED failed — insufficient RLIMIT_MEMLOCK";
  }

  EXPECT_TRUE(opt->is_valid()); // NOLINT(bugprone-unchecked-optional-access)
  auto *ptr = static_cast<std::uint64_t *>(
      opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  *ptr = 77;
  EXPECT_EQ(77U, *ptr);
}

// ============================================================================
// map_file auto-detect size overload
// ============================================================================

TEST_F(MmapRegionFileTest, MapFileAutoDetectSize) {
  // Write known content and set file size.
  constexpr char kData[] = "Auto-size detection test!";
  const std::size_t data_len = sizeof(kData);

  ::ftruncate(fd_, 4096);
  ASSERT_EQ(static_cast<ssize_t>(data_len), ::write(fd_, kData, data_len));

  // Use the auto-detect overload (no explicit size).
  auto opt = MmapRegion::map_file(fd_, false);
  ASSERT_TRUE(opt.has_value());
  EXPECT_EQ(4096U, opt->size()); // NOLINT(bugprone-unchecked-optional-access)

  // Verify content matches.
  const auto *mapped = static_cast<const char *>(
      opt->get()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(0, std::memcmp(mapped, kData, data_len));
}

TEST(MmapRegionTest, MapFileAutoDetectRejectsNonRegularFile) {
  // Pipes are not regular files — fstat reports st_size=0 and mmap
  // behavior is undefined. The auto-detect overload should reject them
  // via S_ISREG check before attempting mmap.
  int pipefd[2];
  ASSERT_EQ(0, ::pipe(pipefd));

  auto opt = MmapRegion::map_file(pipefd[0], false);
  EXPECT_FALSE(opt.has_value());

  ::close(pipefd[0]);
  ::close(pipefd[1]);
}

TEST(MmapRegionTest, MapFileAutoDetectEmptyFileFails) {
  // Create an empty temp file.
  char path[] = "/tmp/mmap_empty_XXXXXX";
  const int fd = ::mkstemp(path);
  ASSERT_GE(fd, 0);

  // Don't write anything — file is empty (0 bytes).
  auto opt = MmapRegion::map_file(fd, false);
  EXPECT_FALSE(opt.has_value());

  ::close(fd);
  ::unlink(path);
}
