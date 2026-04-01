/**
 * @file async_drain_loop_test.cpp
 * @brief Tests for AsyncDrainLoop — generic SPSC drain loop infrastructure.
 *
 * Uses a minimal TestEntry type (not the app-specific LogEntry) to verify
 * the generic drain loop: start/stop lifecycle, multi-queue drain,
 * TSC-ordered merge, and final drain on stop.
 */

#include "sys/log/async_drain_loop.hpp"
#include "sys/memory/mmap_region.hpp"
#include "sys/nano_clock.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

// ---------------------------------------------------------------------------
// Minimal test entry — satisfies DrainableEntry concept
// ---------------------------------------------------------------------------

struct TestEntry {
  std::uint64_t tsc_timestamp{0};
  std::uint32_t value{0};
  char tag[4]{};
};

static_assert(std::is_trivially_copyable_v<TestEntry>);
static_assert(std::is_trivially_destructible_v<TestEntry>);

// ---------------------------------------------------------------------------
// Minimal formatter — writes "tsc=<ts> val=<value> tag=<tag>\n"
// ---------------------------------------------------------------------------

struct TestFormatter {
  std::size_t operator()(const TestEntry &entry,
                         const mk::sys::TscCalibration & /*tsc_cal*/,
                         std::span<char> buf) const noexcept {
    auto result =
        std::snprintf(buf.data(), buf.size(), "tsc=%lu val=%u tag=%.4s\n",
                      static_cast<unsigned long>(entry.tsc_timestamp),
                      entry.value, entry.tag);
    // snprintf returns "desired" length on truncation — clamp to actual.
    if (result <= 0) {
      return 0;
    }
    return std::min(static_cast<std::size_t>(result), buf.size() - 1);
  }
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

using Queue = mk::sys::memory::SPSCQueue<TestEntry>;

class AsyncDrainLoopTest : public ::testing::Test {
protected:
  static constexpr std::uint32_t kCapacity = 256;

  void SetUp() override {
    log_path_ = make_temp_path();
    const auto buf_size = Queue::required_buffer_size(kCapacity);
    buf_a_ = mk::sys::memory::MmapRegion::allocate_anonymous(buf_size);
    buf_b_ = mk::sys::memory::MmapRegion::allocate_anonymous(buf_size);
    ASSERT_TRUE(buf_a_.has_value());
    ASSERT_TRUE(buf_b_.has_value());
  }

  void TearDown() override { ::unlink(log_path_.c_str()); }

  static std::string make_temp_path() {
    char tmpl[] = "/tmp/drain_loop_test_XXXXXX";
    const int fd = mkstemp(tmpl);
    if (fd >= 0) {
      ::close(fd);
      ::unlink(tmpl);
    }
    return std::string(tmpl) + ".log";
  }

  static std::string read_file(const std::string &path) {
    const std::ifstream ifs(path);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
  }

  std::string log_path_;
  std::optional<mk::sys::memory::MmapRegion> buf_a_;
  std::optional<mk::sys::memory::MmapRegion> buf_b_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(AsyncDrainLoopTest, StartsAndStopsCleanly) {
  // NOLINTBEGIN(bugprone-unchecked-optional-access) — guarded by SetUp()
  Queue q_a(buf_a_->get(), buf_a_->size(), kCapacity);
  // NOLINTEND(bugprone-unchecked-optional-access)
  mk::sys::log::AsyncDrainLoop<TestEntry, TestFormatter> loop(
      log_path_.c_str());
  loop.register_queue(&q_a);
  loop.start();
  loop.stop();
  EXPECT_EQ(loop.entries_written(), 0);
}

TEST_F(AsyncDrainLoopTest, DrainsSingleQueue) {
  // NOLINTBEGIN(bugprone-unchecked-optional-access) — guarded by SetUp()
  Queue q_a(buf_a_->get(), buf_a_->size(), kCapacity);
  // NOLINTEND(bugprone-unchecked-optional-access)
  mk::sys::log::AsyncDrainLoop<TestEntry, TestFormatter> loop(
      log_path_.c_str());
  loop.register_queue(&q_a);
  loop.start();

  TestEntry e{};
  e.tsc_timestamp = mk::sys::rdtsc();
  e.value = 42;
  std::memcpy(e.tag, "ABCD", 4);
  EXPECT_TRUE(q_a.try_push(e));

  usleep(50'000);
  loop.stop();

  EXPECT_EQ(loop.entries_written(), 1);
  auto content = read_file(log_path_);
  EXPECT_NE(content.find("val=42"), std::string::npos);
  EXPECT_NE(content.find("tag=ABCD"), std::string::npos);
}

TEST_F(AsyncDrainLoopTest, DrainsTwoQueuesInTscOrder) {
  // NOLINTBEGIN(bugprone-unchecked-optional-access) — guarded by SetUp()
  Queue q_a(buf_a_->get(), buf_a_->size(), kCapacity);
  Queue q_b(buf_b_->get(), buf_b_->size(), kCapacity);
  // NOLINTEND(bugprone-unchecked-optional-access)

  mk::sys::log::AsyncDrainLoop<TestEntry, TestFormatter> loop(
      log_path_.c_str());
  loop.register_queue(&q_a);
  loop.register_queue(&q_b);
  loop.start();

  // Push to queue B first (higher TSC), then queue A (lower TSC).
  TestEntry e_b{};
  e_b.tsc_timestamp = 200;
  e_b.value = 2;
  EXPECT_TRUE(q_b.try_push(e_b));

  TestEntry e_a{};
  e_a.tsc_timestamp = 100;
  e_a.value = 1;
  EXPECT_TRUE(q_a.try_push(e_a));

  usleep(50'000);
  loop.stop();

  EXPECT_EQ(loop.entries_written(), 2);

  // Verify TSC-ordered output: val=1 (tsc=100) before val=2 (tsc=200).
  auto content = read_file(log_path_);
  auto pos1 = content.find("val=1");
  auto pos2 = content.find("val=2");
  ASSERT_NE(pos1, std::string::npos);
  ASSERT_NE(pos2, std::string::npos);
  EXPECT_LT(pos1, pos2) << "Entries should be TSC-ordered";
}

TEST_F(AsyncDrainLoopTest, FinalDrainOnStop) {
  // NOLINTBEGIN(bugprone-unchecked-optional-access) — guarded by SetUp()
  Queue q_a(buf_a_->get(), buf_a_->size(), kCapacity);
  // NOLINTEND(bugprone-unchecked-optional-access)

  mk::sys::log::AsyncDrainLoop<TestEntry, TestFormatter> loop(
      log_path_.c_str());
  loop.register_queue(&q_a);
  loop.start();

  constexpr int kCount = 50;
  for (int i = 0; i < kCount; ++i) {
    TestEntry e{};
    e.tsc_timestamp = mk::sys::rdtsc();
    e.value = static_cast<std::uint32_t>(i);
    EXPECT_TRUE(q_a.try_push(e));
  }

  // Stop immediately — drain loop must final-drain.
  loop.stop();
  EXPECT_EQ(loop.entries_written(), kCount);
}

} // namespace
