/**
 * @file signal_logger_test.cpp
 * @brief GTest-based tests for signal_log -- allocation-free, signal-safe
 * logger.
 *
 * Google Test Death/Exit Test additional concepts:
 *
 *   EXPECT_EXIT(statement, predicate, regex):
 *     - Executes statement in a child process (fork).
 *     - predicate specifies the exit condition:
 *       ::testing::ExitedWithCode(n) -- matches exit(n) or _exit(n)
 *       ::testing::KilledBySignal(sig) -- matches signal termination
 *     - regex is matched against stderr output.
 *     - Since signal_log() uses raw write(2) to stderr,
 *       the output is captured by Death/Exit test stderr matching.
 *
 *   EXPECT_NO_FATAL_FAILURE(statement):
 *     - Verifies that statement completes without any ASSERT (fatal) failures.
 *     - Useful for smoke-testing functions that should not crash.
 *
 *   EXPECT_DEATH vs EXPECT_EXIT:
 *     - EXPECT_DEATH: succeeds on abnormal termination (abort, signal, etc.)
 *     - EXPECT_EXIT: requires explicit exit condition. More precise control.
 *
 * Test plan:
 *   1. Basic type logging (int, unsigned, double, string, char)
 *   2. Safety guard (std::string_view explicit cast)
 *   3. Pointer address logging
 *   4. [Exit Test] Signal handler crash scenario (SIGSEGV -> _exit(1))
 */

#include "sys/log/signal_logger.hpp"

#include <gtest/gtest.h>

#include <csignal>
#include <numbers>
#include <string>

// ============================================================================
// Namespace alias
// ============================================================================

namespace log = mk::sys::log;

// ============================================================================
// 1. Basic Type Logging
// ============================================================================
//
// signal_log writes directly to stderr via write(2).
// Here we verify that calling it with various types does not crash.
// (Content verification is possible via Death/Exit test stderr capture.)

TEST(SignalLoggerTest, BasicTypesDoNotCrash) {
  const int i = -12345;
  const unsigned long u = 9876543210UL;
  const double d = std::numbers::pi;
  const char *str = "Hello HFT";
  const char c = '!';

  EXPECT_NO_FATAL_FAILURE({
    log::signal_log("Integer : ", i, "\n");
    log::signal_log("Unsigned: ", u, "\n");
    log::signal_log("Double  : ", d, "\n");
    log::signal_log("String  : ", str, c, "\n");
    log::signal_log("Mixed   : ", "Price=", 10500.5, ", Qty=", 50, "\n");
  });
}

// ============================================================================
// 2. Safety Guard (std::string_view explicit cast)
// ============================================================================
//
// Passing std::string directly to signal_log triggers a compile-time error
// (static_assert). Explicit conversion to std::string_view is required.
// This test verifies the correct usage compiles and runs without issues.

TEST(SignalLoggerTest, StringViewExplicitCastWorks) {
  const std::string heavy_string = "This causes allocation if passed by value!";

  // std::string -> std::string_view explicit conversion: safe
  EXPECT_NO_FATAL_FAILURE({
    log::signal_log("string_view: ", std::string_view(heavy_string), "\n");
  });
}

// ============================================================================
// 3. Pointer Address Logging
// ============================================================================

TEST(SignalLoggerTest, PointerAddressLogging) {
  int x = 42;
  EXPECT_NO_FATAL_FAILURE({ log::signal_log("Address: ", &x, "\n"); });
}

// ============================================================================
// 4. [Exit Test] Signal Handler Crash Scenario
// ============================================================================
//
// Deliberately triggers SIGSEGV, then the signal handler logs a message
// via signal_log and exits with _exit(1).
//
// EXPECT_EXIT: runs in a forked child, so the parent is not affected.
// Verifies _exit(1) with ExitedWithCode(1) and matches "CRASH DETECTED"
// in stderr output.

namespace {

void test_crash_handler(int signo) {
  log::signal_log("!!! CRASH DETECTED !!!\n");
  log::signal_log("  Signal Number: ", signo, "\n");
  _exit(1);
}

} // namespace

TEST(SignalLoggerDeathTest, CrashHandlerLogsAndExits) {
  EXPECT_EXIT(
      {
        std::signal(SIGSEGV, test_crash_handler);
        volatile int *p = nullptr;
        *p = 42; // Triggers SIGSEGV
      },
      ::testing::ExitedWithCode(1), "CRASH DETECTED");
}
