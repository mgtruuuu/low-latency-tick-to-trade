/**
 * @file scoped_fd_test.cpp
 * @brief Adversarial tests for ScopedFd — RAII file descriptor wrapper.
 *
 * Test strategy:
 *   - Uses pipe() to create real file descriptors (no mocking).
 *   - Verifies RAII close, move semantics, release, reset.
 *   - Death tests verify no UB on edge cases.
 *   - fcntl(fd, F_GETFD) is used to check if an fd is still open:
 *     returns 0 if open, -1 with errno==EBADF if closed.
 */

#include "net/scoped_fd.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace mk::net {
namespace {

// Helper: returns true if the fd is open (valid).
bool is_fd_open(int fd) { return fcntl(fd, F_GETFD) != -1 || errno != EBADF; }

// Helper: creates a pipe and returns {read_end, write_end}.
// Aborts on failure — callers depend on valid fds.
std::pair<int, int> make_pipe() {
  int fds[2] = {-1, -1};
  if (pipe(fds) != 0) {
    std::abort();
  }
  return {fds[0], fds[1]};
}

// ============================================================================
// Basic RAII
// ============================================================================

TEST(ScopedFdTest, DefaultConstructsInvalid) {
  const ScopedFd fd;
  EXPECT_FALSE(fd.is_valid());
  EXPECT_EQ(fd.get(), -1);
  EXPECT_FALSE(static_cast<bool>(fd));
}

TEST(ScopedFdTest, ConstructsWithValidFd) {
  auto [r, w] = make_pipe();
  const ScopedFd w_guard(w); // RAII: don't leak the write end.

  const ScopedFd fd(r);
  EXPECT_TRUE(fd.is_valid());
  EXPECT_EQ(fd.get(), r);
  EXPECT_TRUE(static_cast<bool>(fd));
}

TEST(ScopedFdTest, DestructorClosesValidFd) {
  auto [r, w] = make_pipe();
  const ScopedFd w_guard(w);

  int raw_fd;
  {
    const ScopedFd fd(r);
    raw_fd = fd.get();
    ASSERT_TRUE(is_fd_open(raw_fd));
  }
  // After ScopedFd destructor, the fd must be closed.
  EXPECT_FALSE(is_fd_open(raw_fd));
}

TEST(ScopedFdTest, DestructorToleratesInvalidFd) {
  // Default-constructed ScopedFd with fd_=-1 must not crash on destruction.
  const ScopedFd fd;
  // Destructor runs at end of scope — no crash = pass.
}

TEST(ScopedFdTest, ConstructWithNegativeOneFd) {
  // Explicitly passing -1 is allowed (same as default).
  const ScopedFd fd(-1);
  EXPECT_FALSE(fd.is_valid());
}

// ============================================================================
// Move Semantics
// ============================================================================

TEST(ScopedFdTest, MoveConstructTransfersOwnership) {
  auto [r, w] = make_pipe();
  const ScopedFd w_guard(w);

  ScopedFd a(r);
  const int raw_fd = a.get();

  const ScopedFd b(std::move(a));
  // Source is invalidated.
  EXPECT_FALSE(a.is_valid()); // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(a.get(), -1);
  // Destination owns the fd.
  EXPECT_TRUE(b.is_valid());
  EXPECT_EQ(b.get(), raw_fd);
  EXPECT_TRUE(is_fd_open(raw_fd));
}

TEST(ScopedFdTest, MoveAssignTransfersOwnershipAndClosesPrevious) {
  auto [r1, w1] = make_pipe();
  auto [r2, w2] = make_pipe();
  const ScopedFd w1_guard(w1);
  const ScopedFd w2_guard(w2);

  ScopedFd a(r1);
  ScopedFd b(r2);
  const int old_fd = b.get();

  b = std::move(a);
  // a is invalidated.
  EXPECT_FALSE(a.is_valid()); // NOLINT(bugprone-use-after-move)
  // b now owns r1.
  EXPECT_EQ(b.get(), r1);
  // The previous fd (r2) must be closed.
  EXPECT_FALSE(is_fd_open(old_fd));
}

TEST(ScopedFdTest, SelfMoveAssignDoesNotClose) {
  auto [r, w] = make_pipe();
  const ScopedFd w_guard(w);

  ScopedFd a(r);
  // Self-move-assign should be a no-op (fd remains valid).
  a = std::move(a);
  EXPECT_TRUE(a.is_valid());
  EXPECT_TRUE(is_fd_open(r));
}

TEST(ScopedFdTest, MoveFromInvalidToInvalid) {
  ScopedFd a;
  ScopedFd b;
  b = std::move(a);
  EXPECT_FALSE(a.is_valid()); // NOLINT(bugprone-use-after-move)
  EXPECT_FALSE(b.is_valid());
}

TEST(ScopedFdTest, MoveFromInvalidToValid) {
  auto [r, w] = make_pipe();
  const ScopedFd w_guard(w);

  ScopedFd a; // invalid
  ScopedFd b(r);
  const int old_fd = b.get();

  b = std::move(a);
  EXPECT_FALSE(a.is_valid()); // NOLINT(bugprone-use-after-move)
  EXPECT_FALSE(b.is_valid());
  // r was closed when b was overwritten.
  EXPECT_FALSE(is_fd_open(old_fd));
}

// ============================================================================
// release()
// ============================================================================

TEST(ScopedFdTest, ReleaseReturnsRawFdAndInvalidates) {
  auto [r, w] = make_pipe();
  const ScopedFd w_guard(w);

  ScopedFd fd(r);
  const int released = fd.release();
  EXPECT_EQ(released, r);
  EXPECT_FALSE(fd.is_valid());
  // The fd is NOT closed — caller owns it.
  EXPECT_TRUE(is_fd_open(released));
  // Clean up manually.
  ::close(released);
}

TEST(ScopedFdTest, ReleaseOnInvalidReturnsNegativeOne) {
  ScopedFd fd;
  const int released = fd.release();
  EXPECT_EQ(released, -1);
  EXPECT_FALSE(fd.is_valid());
}

// ============================================================================
// reset()
// ============================================================================

TEST(ScopedFdTest, ResetClosesPreviousAndStoresNew) {
  auto [r1, w1] = make_pipe();
  auto [r2, w2] = make_pipe();
  const ScopedFd w1_guard(w1);
  const ScopedFd w2_guard(w2);

  ScopedFd fd(r1);
  fd.reset(r2);
  EXPECT_EQ(fd.get(), r2);
  EXPECT_FALSE(is_fd_open(r1));
  EXPECT_TRUE(is_fd_open(r2));
}

TEST(ScopedFdTest, ResetToNegativeOneClosesAndInvalidates) {
  auto [r, w] = make_pipe();
  const ScopedFd w_guard(w);

  ScopedFd fd(r);
  fd.reset(); // default arg is -1
  EXPECT_FALSE(fd.is_valid());
  EXPECT_FALSE(is_fd_open(r));
}

TEST(ScopedFdTest, ResetOnInvalidToInvalidIsNoop) {
  ScopedFd fd;
  fd.reset(-1);
  EXPECT_FALSE(fd.is_valid());
}

TEST(ScopedFdTest, SelfResetWithInvalidIsNoop) {
  // reset(-1) on an already-invalid fd is fine — no assert.
  ScopedFd fd;
  fd.reset(-1);
  EXPECT_FALSE(fd.is_valid());
}

// ============================================================================
// No Copy
// ============================================================================

TEST(ScopedFdTest, NotCopyable) {
  EXPECT_FALSE(std::is_copy_constructible_v<ScopedFd>);
  EXPECT_FALSE(std::is_copy_assignable_v<ScopedFd>);
}

TEST(ScopedFdTest, IsMovable) {
  EXPECT_TRUE(std::is_move_constructible_v<ScopedFd>);
  EXPECT_TRUE(std::is_move_assignable_v<ScopedFd>);
}

// ============================================================================
// fd Leak Regression
// ============================================================================

TEST(ScopedFdTest, MultiplePipesNoLeak) {
  // Create and destroy many ScopedFd objects. If there's an fd leak,
  // we'll eventually hit the per-process fd limit and pipe() will fail.
  for (int i = 0; i < 500; ++i) {
    auto [r, w] = make_pipe();
    const ScopedFd a(r);
    const ScopedFd b(w);
    // Both closed at end of each iteration.
  }
}

// ============================================================================
// swap()
// ============================================================================

TEST(ScopedFdTest, SwapExchangesFds) {
  auto [r1, w1] = make_pipe();
  auto [r2, w2] = make_pipe();
  const ScopedFd w1_guard(w1);
  const ScopedFd w2_guard(w2);

  ScopedFd a(r1);
  ScopedFd b(r2);

  a.swap(b);
  EXPECT_EQ(a.get(), r2);
  EXPECT_EQ(b.get(), r1);
  // Both fds still open — no close happened.
  EXPECT_TRUE(is_fd_open(r1));
  EXPECT_TRUE(is_fd_open(r2));
}

TEST(ScopedFdTest, AdlSwapWorks) {
  auto [r1, w1] = make_pipe();
  auto [r2, w2] = make_pipe();
  const ScopedFd w1_guard(w1);
  const ScopedFd w2_guard(w2);

  ScopedFd a(r1);
  ScopedFd b(r2);

  // ADL: unqualified swap should find the friend function.
  using std::swap;
  swap(a, b);
  EXPECT_EQ(a.get(), r2);
  EXPECT_EQ(b.get(), r1);
}

TEST(ScopedFdTest, SwapWithInvalid) {
  auto [r, w] = make_pipe();
  const ScopedFd w_guard(w);

  ScopedFd a(r);
  ScopedFd b; // invalid

  a.swap(b);
  EXPECT_FALSE(a.is_valid());
  EXPECT_EQ(b.get(), r);
  EXPECT_TRUE(is_fd_open(r));
}

// ============================================================================
// Death Tests (Debug only)
// ============================================================================

#ifndef NDEBUG

using ScopedFdDeathTest = ::testing::Test;

TEST_F(ScopedFdDeathTest, SelfResetWithValidFdAsserts) {
  EXPECT_DEATH(
      {
        auto fds = make_pipe();
        ScopedFd fd(fds.first);
        ::close(fds.second);
        fd.reset(fds.first); // self-reset with own valid fd
      },
      "reset.*own fd.*likely a bug");
}

#endif

} // namespace
} // namespace mk::net
