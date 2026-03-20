/**
 * @file affinity_test.cpp
 * @brief GTest-based tests for thread affinity (CPU pinning).
 *
 * Google Test concepts used:
 *
 *   EXPECT_GE(a, b) / EXPECT_LE(a, b):
 *     Range comparisons. Used here to verify core indices are within
 *     valid bounds.
 *
 *   GTEST_SKIP():
 *     Skips the current test with a message. Used when the system has
 *     only 1 core (e.g., CI container), making pinning tests meaningless.
 *
 * Test plan:
 *   1. get_available_cores returns a positive count
 *   2. get_current_core returns a valid index
 *   3. pin_current_thread succeeds and actually moves the thread
 *   4. pin_current_thread with invalid core_id fails
 *   5. Pin from a spawned thread (not just main)
 */

#include "sys/thread/affinity.hpp"

#include <gtest/gtest.h>

#include <pthread.h>
#include <sched.h>
#include <thread>

namespace affinity = mk::sys::thread;

// ============================================================================
// 1. get_available_cores
// ============================================================================

TEST(AffinityTest, AvailableCoresIsPositive) {
  const std::uint32_t cores = affinity::get_available_cores();
  EXPECT_GE(cores, 1U);
}

// ============================================================================
// 2. get_current_core
// ============================================================================

TEST(AffinityTest, GetCurrentCoreReturnsValidIndex) {
  const int core = affinity::get_current_core();
  EXPECT_GE(core, 0);
}

// ============================================================================
// 3. pin_current_thread succeeds
// ============================================================================
//
// After pinning to a specific core, get_current_core() must return that core.
// Skipped on single-core systems where the thread is already on core 0.

TEST(AffinityTest, PinCurrentThreadSucceeds) {
  const std::uint32_t cores = affinity::get_available_cores();
  if (cores < 2) {
    GTEST_SKIP() << "Need at least 2 cores to test pinning";
  }

  // Save original affinity mask so we can restore it after the test.
  // Without restoration, subsequent tests would see only 1 available core
  // because gtest reuses the same thread for all TEST() cases.
  cpu_set_t original;
  CPU_ZERO(&original);
  pthread_getaffinity_np(pthread_self(), sizeof(original), &original);

  // Pin to core 1 (avoid core 0, which is often reserved for OS tasks)
  const int err = affinity::pin_current_thread(1);
  EXPECT_EQ(0, err);

  const int current = affinity::get_current_core();
  EXPECT_EQ(1, current);

  // Restore original affinity
  pthread_setaffinity_np(pthread_self(), sizeof(original), &original);
}

// ============================================================================
// 4. pin_current_thread with invalid core fails
// ============================================================================

TEST(AffinityTest, PinToInvalidCoreFails) {
  // Core 99999 almost certainly does not exist
  const int err = affinity::pin_current_thread(99999);
  EXPECT_NE(0, err);
}

// ============================================================================
// 5. Pin from a spawned thread
// ============================================================================
//
// Verify that pinning works from any thread, not just main.
// Collect results via variables, verify on main thread.

TEST(AffinityTest, PinFromSpawnedThread) {
  const std::uint32_t cores = affinity::get_available_cores();
  if (cores < 2) {
    GTEST_SKIP() << "Need at least 2 cores to test pinning";
  }

  int pin_err = -1;
  int observed_core = -1;

  std::thread worker([&] {
    pin_err = affinity::pin_current_thread(0);
    observed_core = affinity::get_current_core();
  });
  worker.join();

  EXPECT_EQ(0, pin_err);
  EXPECT_EQ(0, observed_core);
}
