/**
 * @file socket_base_test.cpp
 * @brief Tests for SocketBase — non-virtual base class for socket wrappers.
 *
 * Test strategy:
 *   - Uses socketpair(AF_UNIX, SOCK_STREAM, 0) for connected fd pairs.
 *   - Tests SocketBase functionality directly via TcpSocket (which inherits
 *     SocketBase without adding any overrides to the tested methods).
 *   - Verifies: construction/abort, observers, move semantics, reset/release,
 *     blocking/nonblocking mode, socket options.
 *   - Death tests verify abort on invalid fd.
 *
 * Note: SocketBase cannot be instantiated directly (protected constructor
 * would require a derived class). We test through TcpSocket, which inherits
 * all SocketBase methods unchanged.
 */

#include "net/socket_base.hpp"
#include "net/tcp_socket.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace mk::net {
namespace {

// Helper: creates a connected AF_UNIX socketpair (SOCK_STREAM).
std::pair<int, int> make_socketpair() {
  int fds[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    std::abort();
  }
  return {fds[0], fds[1]};
}

// Helper: returns true if the fd is open (valid).
bool is_fd_open(int fd) { return fcntl(fd, F_GETFD) != -1 || errno != EBADF; }

// ============================================================================
// Construction — abort on invalid fd
// ============================================================================

using SocketBaseDeathTest = ::testing::Test;

TEST_F(SocketBaseDeathTest, AbortOnInvalidFd) {
  EXPECT_DEATH({ const TcpSocket sock(-1); }, "FATAL.*Socket.*invalid fd");
}

// ============================================================================
// Observers
// ============================================================================

TEST(SocketBaseTest, GetReturnsUnderlyingFd) {
  auto [a, b] = make_socketpair();
  ::close(b);

  const TcpSocket sock(a);
  EXPECT_EQ(sock.get(), a);
}

TEST(SocketBaseTest, IsValidReturnsTrueForValidFd) {
  auto [a, b] = make_socketpair();
  ::close(b);

  const TcpSocket sock(a);
  EXPECT_TRUE(sock.is_valid());
}

TEST(SocketBaseTest, BoolConversionMatchesIsValid) {
  auto [a, b] = make_socketpair();
  ::close(b);

  const TcpSocket sock(a);
  EXPECT_TRUE(static_cast<bool>(sock));
}

TEST(SocketBaseTest, EqualityComparesUnderlyingFds) {
  auto [a1, b1] = make_socketpair();
  auto [a2, b2] = make_socketpair();
  ::close(b1);
  ::close(b2);

  const TcpSocket sock1(a1);
  const TcpSocket sock2(a2);
  EXPECT_FALSE(sock1 == sock2);
}

// ============================================================================
// Release / Reset
// ============================================================================

TEST(SocketBaseTest, ReleaseTransfersOwnership) {
  auto [a, b] = make_socketpair();
  ::close(b);

  TcpSocket sock(a);
  const int released = sock.release();
  EXPECT_EQ(released, a);
  EXPECT_FALSE(sock.is_valid());
  EXPECT_EQ(sock.get(), -1);

  // Caller now owns the fd — must close manually.
  EXPECT_TRUE(is_fd_open(released));
  ::close(released);
}

TEST(SocketBaseTest, ResetClosesCurrentFd) {
  auto [a, b] = make_socketpair();
  ::close(b);

  TcpSocket sock(a);
  const int old_fd = sock.get();
  sock.reset();

  EXPECT_FALSE(sock.is_valid());
  EXPECT_FALSE(is_fd_open(old_fd));
}

TEST(SocketBaseTest, ResetWithNewFd) {
  auto [a, b] = make_socketpair();
  auto [c, d] = make_socketpair();
  ::close(b);
  ::close(d);

  TcpSocket sock(a);
  const int old_fd = sock.get();
  sock.reset(c);

  EXPECT_EQ(sock.get(), c);
  EXPECT_TRUE(sock.is_valid());
  EXPECT_FALSE(is_fd_open(old_fd));
}

// ============================================================================
// Move semantics
// ============================================================================

TEST(SocketBaseTest, MoveConstructTransfersOwnership) {
  auto [a, b] = make_socketpair();
  ::close(b);

  TcpSocket src(a);
  const TcpSocket dst(std::move(src));

  EXPECT_EQ(dst.get(), a);
  EXPECT_TRUE(dst.is_valid());
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_FALSE(src.is_valid());
}

TEST(SocketBaseTest, MoveAssignTransfersOwnership) {
  auto [a, b] = make_socketpair();
  auto [c, d] = make_socketpair();
  ::close(b);
  ::close(d);

  TcpSocket src(a);
  TcpSocket dst(c);
  const int old_dst_fd = dst.get();

  dst = std::move(src);

  EXPECT_EQ(dst.get(), a);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_FALSE(src.is_valid());
  // Old fd from dst should be closed.
  EXPECT_FALSE(is_fd_open(old_dst_fd));
}

// ============================================================================
// Blocking / Nonblocking mode
// ============================================================================

TEST(SocketBaseTest, SetNonblockingSetsFlag) {
  auto [a, b] = make_socketpair();
  ::close(b);

  TcpSocket sock(a);
  ASSERT_TRUE(sock.set_nonblocking());

  // Verify O_NONBLOCK is set.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const int flags = fcntl(sock.get(), F_GETFL, 0);
  EXPECT_NE(flags & O_NONBLOCK, 0);
}

TEST(SocketBaseTest, SetBlockingClearsFlag) {
  auto [a, b] = make_socketpair();
  ::close(b);

  TcpSocket sock(a);
  // First set nonblocking, then clear it.
  ASSERT_TRUE(sock.set_nonblocking());
  ASSERT_TRUE(sock.set_blocking());

  // Verify O_NONBLOCK is cleared.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const int flags = fcntl(sock.get(), F_GETFL, 0);
  EXPECT_EQ(flags & O_NONBLOCK, 0);
}

TEST(SocketBaseTest, SetNonblockingThenBlockingRoundTrip) {
  auto [a, b] = make_socketpair();
  ::close(b);

  TcpSocket sock(a);

  // Get initial flags.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const int initial_flags = fcntl(sock.get(), F_GETFL, 0);

  // Set nonblocking.
  ASSERT_TRUE(sock.set_nonblocking());
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  int flags = fcntl(sock.get(), F_GETFL, 0);
  EXPECT_NE(flags & O_NONBLOCK, 0);

  // Set blocking again — should restore initial state.
  ASSERT_TRUE(sock.set_blocking());
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  flags = fcntl(sock.get(), F_GETFL, 0);
  EXPECT_EQ(flags & O_NONBLOCK, 0);
  EXPECT_EQ(flags, initial_flags);
}

// ============================================================================
// Socket options
// ============================================================================

TEST(SocketBaseTest, SetReuseaddrSucceeds) {
  auto [a, b] = make_socketpair();
  ::close(b);

  TcpSocket sock(a);
  EXPECT_TRUE(sock.set_reuseaddr(true));

  // Verify via getsockopt.
  int val = 0;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &val, &len), 0);
  EXPECT_NE(val, 0);
}

TEST(SocketBaseTest, SetReuseaddrDisable) {
  auto [a, b] = make_socketpair();
  ::close(b);

  TcpSocket sock(a);
  ASSERT_TRUE(sock.set_reuseaddr(true));
  ASSERT_TRUE(sock.set_reuseaddr(false));

  int val = 1;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &val, &len), 0);
  EXPECT_EQ(val, 0);
}

TEST(SocketBaseTest, SetReuseportSucceeds) {
  // SO_REUSEPORT is not supported on AF_UNIX — use a real AF_INET socket.
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_NE(fd, -1);

  TcpSocket sock(fd);
  EXPECT_TRUE(sock.set_reuseport(true));

  int val = 0;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), SOL_SOCKET, SO_REUSEPORT, &val, &len), 0);
  EXPECT_NE(val, 0);
}

TEST(SocketBaseTest, SetSndbufSucceeds) {
  auto [a, b] = make_socketpair();
  ::close(b);

  TcpSocket sock(a);
  constexpr int kRequestedSize = 32768;
  EXPECT_TRUE(sock.set_sndbuf(kRequestedSize));

  // Kernel doubles the value — verify it's at least what we requested.
  int val = 0;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), SOL_SOCKET, SO_SNDBUF, &val, &len), 0);
  EXPECT_GE(val, kRequestedSize);
}

TEST(SocketBaseTest, SetRcvbufSucceeds) {
  auto [a, b] = make_socketpair();
  ::close(b);

  TcpSocket sock(a);
  constexpr int kRequestedSize = 32768;
  EXPECT_TRUE(sock.set_rcvbuf(kRequestedSize));

  // Kernel doubles the value — verify it's at least what we requested.
  int val = 0;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), SOL_SOCKET, SO_RCVBUF, &val, &len), 0);
  EXPECT_GE(val, kRequestedSize);
}

// ============================================================================
// Destructor closes fd
// ============================================================================

TEST(SocketBaseTest, DestructorClosesFd) {
  auto [a, b] = make_socketpair();
  ::close(b);

  int raw_fd;
  {
    const TcpSocket sock(a);
    raw_fd = sock.get();
    ASSERT_TRUE(is_fd_open(raw_fd));
  }
  EXPECT_FALSE(is_fd_open(raw_fd));
}

} // namespace
} // namespace mk::net
