/**
 * @file global_new_delete_test.cpp
 * @brief GTest-based tests for global new/delete hot path guard.
 *
 * Google Test Death Test concepts:
 *
 *   1. EXPECT_DEATH(statement, regex):
 *      - Executes statement in a child process (fork).
 *      - Succeeds if the process terminates abnormally (abort, signal).
 *      - regex is matched against stderr output.
 *
 *   2. EXPECT_EXIT(statement, predicate, regex):
 *      - More precise than EXPECT_DEATH.
 *      - ::testing::ExitedWithCode(n) -- matches exit(n)
 *      - ::testing::KilledBySignal(sig) -- matches signal termination
 *
 *   3. Death tests are fork-based, so they do NOT affect the parent process.
 *      One test can abort without breaking others.
 *
 *   4. By convention, death test suite names end with "*DeathTest".
 *      gtest runs these suites first for fork safety.
 *
 * Test plan:
 *   1. Warm-up allocations succeed (hot path off)
 *   2. Hot path mode toggle (on/off)
 *   3. Stack variables work during hot path
 *   4. [Death Test] new during hot path aborts (Debug only)
 *   5. [Death Test] delete during hot path aborts (Debug only)
 */

#include "sys/memory/global_new_delete.hpp"
#include "sys/thread/hot_path_control.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

// ============================================================================
// Linker Anchor via Global Test Environment
// ============================================================================
//
// Since gtest_main provides main(), we use ::testing::Environment to call
// install_global_memory_guard() once before all tests run.
//
// ::testing::Environment:
//   A global environment object managed by gtest.
//   SetUp() is called before all test suites; TearDown() after all tests.
//   AddGlobalTestEnvironment() transfers ownership to gtest.

class GlobalMemoryGuardEnv : public ::testing::Environment {
public:
  void SetUp() override { mk::sys::memory::install_global_memory_guard(); }
};

// NOLINTNEXTLINE(readability-identifier-naming)
static auto *const g_env [[maybe_unused]] =
    ::testing::AddGlobalTestEnvironment(new GlobalMemoryGuardEnv);

// ============================================================================
// 1. Warm-up Allocations Succeed
// ============================================================================
//
// Verifies that normal heap allocations work when hot path mode is off.
// At this point, hot_path_mode == false (default).

TEST(GlobalNewDeleteTest, WarmupAllocationsSucceed) {
  std::vector<int> v;
  v.reserve(1000);
  for (int i = 0; i < 100; ++i) {
    v.push_back(i);
  }
  EXPECT_EQ(100U, v.size());
  EXPECT_GE(v.capacity(), 1000U);
}

// ============================================================================
// 2. Hot Path Mode Toggle
// ============================================================================
//
// IMPORTANT: While hot path is active, gtest macros may internally allocate.
// Capture the value first, disable hot path, then verify with EXPECT.

TEST(GlobalNewDeleteTest, HotPathModeToggle) {
  EXPECT_FALSE(mk::sys::thread::is_hot_path_mode());

  mk::sys::thread::set_hot_path_mode(true);

#ifndef NDEBUG
  // Avoid calling gtest macros while hot path is active (internal allocation)
  const bool was_active = mk::sys::thread::is_hot_path_mode();
  mk::sys::thread::set_hot_path_mode(false);
  EXPECT_TRUE(was_active);
#else
  mk::sys::thread::set_hot_path_mode(false);
#endif

  EXPECT_FALSE(mk::sys::thread::is_hot_path_mode());
}

// ============================================================================
// 3. Stack Variables Work During Hot Path
// ============================================================================
//
// Stack variables do not involve heap allocation, so they are safe.

TEST(GlobalNewDeleteTest, StackVariablesWorkDuringHotPath) {
  mk::sys::thread::set_hot_path_mode(true);

  const int price = 100;
  const int quantity = 50;
  const double total = static_cast<double>(price) * quantity;

  mk::sys::thread::set_hot_path_mode(false);

  EXPECT_EQ(5000.0, total);
}

// ============================================================================
// 4. [Death Test] Allocation During Hot Path Aborts
// ============================================================================
//
// The hot path guard only works in Debug builds (#ifndef NDEBUG).
// In Release builds, the guard is a no-op, so we skip these tests.

#ifndef NDEBUG

TEST(GlobalNewDeleteDeathTest, AllocationDuringHotPathAborts) {
  EXPECT_DEATH(
      {
        mk::sys::memory::install_global_memory_guard();
        mk::sys::thread::set_hot_path_mode(true);
        // std::string ctor calls operator new, which triggers the guard
        const std::string s = "This allocation should trigger abort";
        (void)s;
      },
      "Hot Path Allocation Violation");
}

// ============================================================================
// 5. [Death Test] Deallocation During Hot Path Aborts
// ============================================================================

TEST(GlobalNewDeleteDeathTest, DeallocationDuringHotPathAborts) {
  EXPECT_DEATH(
      {
        mk::sys::memory::install_global_memory_guard();
        // Allocate outside hot path
        auto *p = new int(42);
        // Enter hot path, then attempt to delete
        mk::sys::thread::set_hot_path_mode(true);
        delete p;
      },
      "Hot Path Deallocation Violation");
}

#else

// Release build: guard is inactive. Documentation-only disabled test.
// DISABLED_ prefix makes gtest skip it automatically.
TEST(GlobalNewDeleteTest, DISABLED_HotPathGuardIsNoOpInRelease) {
  GTEST_SKIP() << "Hot path guard is disabled in Release builds";
}

#endif
