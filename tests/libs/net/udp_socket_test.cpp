/**
 * @file udp_socket_test.cpp
 * @brief Tests for UdpSocket — RAII UDP datagram socket wrapper.
 *
 * Test strategy:
 *   - Uses loopback UDP sockets bound to kernel-assigned ports.
 *     No external network needed — everything is in-kernel, deterministic.
 *   - Verifies sendto/recvfrom, blocking/nonblocking, message boundaries.
 *   - Death tests: abort on invalid fd, assert on null buf / len==0.
 */

#include "net/udp_socket.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace mk::net {
namespace {

// Helper: creates a UDP socket bound to loopback on a kernel-assigned port.
// Returns the UdpSocket and the bound address (with port filled in).
std::pair<UdpSocket, sockaddr_in> make_bound_udp() {
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    std::abort();
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0; // kernel assigns

  if (::bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) !=
      0) {
    ::close(fd);
    std::abort();
  }

  // Retrieve the kernel-assigned port.
  socklen_t addr_len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &addr_len) != 0) {
    ::close(fd);
    std::abort();
  }

  return {UdpSocket(fd), addr};
}

// Helper: creates an unbound UDP socket (for sendto without bind).
UdpSocket make_udp() {
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    std::abort();
  }
  return UdpSocket(fd);
}

// ============================================================================
// Construction / Destruction
// ============================================================================

using UdpSocketDeathTest = ::testing::Test;

TEST_F(UdpSocketDeathTest, AbortOnInvalidFd) {
  EXPECT_DEATH({ const UdpSocket sock(-1); }, "FATAL.*Socket.*invalid fd");
}

TEST(UdpSocketTest, ConstructWithValidFd) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  ASSERT_NE(fd, -1);

  const UdpSocket sock(fd);
  EXPECT_TRUE(sock.is_valid());
  EXPECT_EQ(sock.get(), fd);
  EXPECT_TRUE(static_cast<bool>(sock));
}

TEST(UdpSocketTest, DestructorClosesFd) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  ASSERT_NE(fd, -1);

  int raw_fd;
  {
    const UdpSocket sock(fd);
    raw_fd = sock.get();
  }
  // After destruction, fd must be closed.
  EXPECT_EQ(fcntl(raw_fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
}

// ============================================================================
// Move Semantics
// ============================================================================

TEST(UdpSocketTest, MoveConstructTransfersOwnership) {
  auto [sock, addr] = make_bound_udp();
  const int original_fd = sock.get();

  const UdpSocket moved(std::move(sock));
  EXPECT_FALSE(sock.is_valid()); // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(moved.get(), original_fd);
}

TEST(UdpSocketTest, MoveAssignClosesPrevious) {
  auto [s1, addr1] = make_bound_udp();
  auto [s2, addr2] = make_bound_udp();
  const int old_fd = s2.get();

  s2 = std::move(s1);
  EXPECT_FALSE(s1.is_valid()); // NOLINT(bugprone-use-after-move)
  // Old fd (s2's original) must be closed.
  EXPECT_EQ(fcntl(old_fd, F_GETFD), -1);
}

TEST(UdpSocketTest, NotCopyable) {
  EXPECT_FALSE(std::is_copy_constructible_v<UdpSocket>);
  EXPECT_FALSE(std::is_copy_assignable_v<UdpSocket>);
}

TEST(UdpSocketTest, IsMovable) {
  EXPECT_TRUE(std::is_move_constructible_v<UdpSocket>);
  EXPECT_TRUE(std::is_move_assignable_v<UdpSocket>);
}

// ============================================================================
// Observers / Mutators
// ============================================================================

TEST(UdpSocketTest, ReleaseTransfersOwnership) {
  auto [sock, addr] = make_bound_udp();
  const int original_fd = sock.get();

  const int released_fd = sock.release();
  EXPECT_EQ(released_fd, original_fd);
  EXPECT_EQ(sock.get(), -1);
  EXPECT_FALSE(sock.is_valid());

  // The released fd is still open — caller owns it now.
  EXPECT_NE(fcntl(released_fd, F_GETFD), -1);
  ::close(released_fd);
}

TEST(UdpSocketTest, ResetToInvalid) {
  auto [sock, addr] = make_bound_udp();
  const int old_fd = sock.get();

  sock.reset();
  EXPECT_FALSE(sock.is_valid());
  EXPECT_EQ(fcntl(old_fd, F_GETFD), -1);
}

TEST(UdpSocketTest, EqualityComparison) {
  auto [s1, addr1] = make_bound_udp();
  auto [s2, addr2] = make_bound_udp();
  EXPECT_FALSE(s1 == s2);
}

// ============================================================================
// Blocking / Non-blocking Mode
// ============================================================================

TEST(UdpSocketTest, SetNonblockingAndBack) {
  auto sock = make_udp();

  ASSERT_TRUE(sock.set_nonblocking());
  int flags = fcntl(sock.get(), F_GETFL, 0);
  EXPECT_NE(flags & O_NONBLOCK, 0);

  ASSERT_TRUE(sock.set_blocking());
  flags = fcntl(sock.get(), F_GETFL, 0);
  EXPECT_EQ(flags & O_NONBLOCK, 0);
}

// ============================================================================
// Socket Options
// ============================================================================

TEST(UdpSocketTest, SetReuseaddr) {
  auto sock = make_udp();
  ASSERT_TRUE(sock.set_reuseaddr(true));

  int val = 0;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &val, &len), 0);
  EXPECT_EQ(val, 1);

  ASSERT_TRUE(sock.set_reuseaddr(false));
  ASSERT_EQ(getsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &val, &len), 0);
  EXPECT_EQ(val, 0);
}

TEST(UdpSocketTest, SetReuseport) {
  auto sock = make_udp();
  ASSERT_TRUE(sock.set_reuseport(true));

  int val = 0;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), SOL_SOCKET, SO_REUSEPORT, &val, &len), 0);
  EXPECT_EQ(val, 1);

  ASSERT_TRUE(sock.set_reuseport(false));
  ASSERT_EQ(getsockopt(sock.get(), SOL_SOCKET, SO_REUSEPORT, &val, &len), 0);
  EXPECT_EQ(val, 0);
}

TEST(UdpSocketTest, SetSndbuf) {
  auto sock = make_udp();
  constexpr int kRequestedSize = 32768;
  ASSERT_TRUE(sock.set_sndbuf(kRequestedSize));

  int val = 0;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), SOL_SOCKET, SO_SNDBUF, &val, &len), 0);
  EXPECT_GE(val, kRequestedSize);
}

TEST(UdpSocketTest, SetRcvbuf) {
  auto sock = make_udp();
  constexpr int kRequestedSize = 32768;
  ASSERT_TRUE(sock.set_rcvbuf(kRequestedSize));

  int val = 0;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), SOL_SOCKET, SO_RCVBUF, &val, &len), 0);
  EXPECT_GE(val, kRequestedSize);
}

// ============================================================================
// Sendto / Recvfrom — Blocking
// ============================================================================

TEST(UdpSocketTest, BasicRoundTrip) {
  // A sends to B, B receives and verifies source address == A.
  auto [a, a_addr] = make_bound_udp();
  auto [b, b_addr] = make_bound_udp();

  const char msg[] = "hello UDP";
  auto send_result = a.sendto_blocking(msg, sizeof(msg), b_addr);
  ASSERT_EQ(send_result.status, UdpSocket::SendtoStatus::kOk);
  ASSERT_EQ(send_result.bytes_sent, static_cast<ssize_t>(sizeof(msg)));

  char buf[64]{};
  auto recv_result = b.recvfrom_blocking(buf, sizeof(buf));
  ASSERT_EQ(recv_result.status, UdpSocket::RecvfromStatus::kOk);
  ASSERT_EQ(recv_result.bytes_received, static_cast<ssize_t>(sizeof(msg)));
  EXPECT_STREQ(buf, msg);

  // Verify source address matches A's bound address.
  EXPECT_EQ(recv_result.source_addr.sin_family, AF_INET);
  EXPECT_EQ(recv_result.source_addr.sin_port, a_addr.sin_port);
  EXPECT_EQ(recv_result.source_addr.sin_addr.s_addr, a_addr.sin_addr.s_addr);
}

TEST(UdpSocketTest, MessageBoundaryPreservation) {
  // Two separate sendto calls should produce two separate recvfrom results.
  // This is the fundamental UDP guarantee — message boundaries are preserved.
  auto [sender, sender_addr] = make_bound_udp();
  auto [receiver, receiver_addr] = make_bound_udp();

  const char msg1[] = "first";
  const char msg2[] = "second message";

  auto sr1 = sender.sendto_blocking(msg1, sizeof(msg1), receiver_addr);
  ASSERT_EQ(sr1.status, UdpSocket::SendtoStatus::kOk);

  auto sr2 = sender.sendto_blocking(msg2, sizeof(msg2), receiver_addr);
  ASSERT_EQ(sr2.status, UdpSocket::SendtoStatus::kOk);

  // First recvfrom should return exactly msg1.
  char buf1[64]{};
  auto rr1 = receiver.recvfrom_blocking(buf1, sizeof(buf1));
  ASSERT_EQ(rr1.status, UdpSocket::RecvfromStatus::kOk);
  ASSERT_EQ(rr1.bytes_received, static_cast<ssize_t>(sizeof(msg1)));
  EXPECT_STREQ(buf1, msg1);

  // Second recvfrom should return exactly msg2.
  char buf2[64]{};
  auto rr2 = receiver.recvfrom_blocking(buf2, sizeof(buf2));
  ASSERT_EQ(rr2.status, UdpSocket::RecvfromStatus::kOk);
  ASSERT_EQ(rr2.bytes_received, static_cast<ssize_t>(sizeof(msg2)));
  EXPECT_STREQ(buf2, msg2);
}

TEST(UdpSocketTest, LargeDatagram) {
  // Send a datagram close to the typical UDP limit (just under 64KB).
  // Linux loopback can handle this without fragmentation issues.
  auto [sender, sender_addr] = make_bound_udp();
  auto [receiver, receiver_addr] = make_bound_udp();

  // Use 8KB — safely below the 64KB UDP limit and within default rcvbuf.
  constexpr std::size_t kSize = 8192;
  std::array<char, kSize> send_buf{};
  for (std::size_t i = 0; i < kSize; ++i) {
    send_buf[i] = static_cast<char>(i & 0x7F);
  }

  auto sr = sender.sendto_blocking(send_buf.data(), kSize, receiver_addr);
  ASSERT_EQ(sr.status, UdpSocket::SendtoStatus::kOk);
  ASSERT_EQ(sr.bytes_sent, static_cast<ssize_t>(kSize));

  std::array<char, kSize> recv_buf{};
  auto rr = receiver.recvfrom_blocking(recv_buf.data(), recv_buf.size());
  ASSERT_EQ(rr.status, UdpSocket::RecvfromStatus::kOk);
  ASSERT_EQ(rr.bytes_received, static_cast<ssize_t>(kSize));
  EXPECT_EQ(send_buf, recv_buf);
}

// ============================================================================
// Sendto / Recvfrom — Non-blocking
// ============================================================================

TEST(UdpSocketTest, RecvfromNonblockingWhenNoData) {
  auto [sock, addr] = make_bound_udp();

  char buf[64]{};
  auto result = sock.recvfrom_nonblocking(buf, sizeof(buf));
  EXPECT_EQ(result.status, UdpSocket::RecvfromStatus::kWouldBlock);
  EXPECT_EQ(result.bytes_received, 0);
}

TEST(UdpSocketTest, RecvfromNonblockingWithData) {
  auto [sender, sender_addr] = make_bound_udp();
  auto [receiver, receiver_addr] = make_bound_udp();

  const char msg[] = "nb data";
  auto sr = sender.sendto_blocking(msg, sizeof(msg), receiver_addr);
  ASSERT_EQ(sr.status, UdpSocket::SendtoStatus::kOk);

  char buf[64]{};
  auto rr = receiver.recvfrom_nonblocking(buf, sizeof(buf));
  ASSERT_EQ(rr.status, UdpSocket::RecvfromStatus::kOk);
  ASSERT_EQ(rr.bytes_received, static_cast<ssize_t>(sizeof(msg)));
  EXPECT_STREQ(buf, msg);
}

TEST(UdpSocketTest, SendtoNonblockingSucceeds) {
  auto [sender, sender_addr] = make_bound_udp();
  auto [receiver, receiver_addr] = make_bound_udp();

  const char msg[] = "nb send";
  auto sr = sender.sendto_nonblocking(msg, sizeof(msg), receiver_addr);
  ASSERT_EQ(sr.status, UdpSocket::SendtoStatus::kOk);
  EXPECT_EQ(sr.bytes_sent, static_cast<ssize_t>(sizeof(msg)));

  // Verify data arrived.
  char buf[64]{};
  auto rr = receiver.recvfrom_blocking(buf, sizeof(buf));
  ASSERT_EQ(rr.status, UdpSocket::RecvfromStatus::kOk);
  EXPECT_STREQ(buf, msg);
}

TEST(UdpSocketTest, RecvfromNonblockingOnBlockingFdReturnsWouldBlock) {
  // Do NOT call set_nonblocking() — fd is in default blocking mode.
  // MSG_DONTWAIT must ensure recvfrom_nonblocking returns immediately.
  auto [sock, addr] = make_bound_udp();

  char buf[64]{};
  auto result = sock.recvfrom_nonblocking(buf, sizeof(buf));
  EXPECT_EQ(result.status, UdpSocket::RecvfromStatus::kWouldBlock);
}

// ============================================================================
// Assert tests (Debug only)
// ============================================================================

#ifndef NDEBUG

using UdpSocketAssertDeathTest = ::testing::Test;

// Helper to get a bound udp socket + address without structured bindings
// (structured bindings contain commas that confuse the EXPECT_DEATH macro).
void death_test_sendto_blocking_null() {
  auto pair = make_bound_udp();
  (void)pair.first.sendto_blocking(nullptr, 1, pair.second);
}
void death_test_sendto_blocking_len0() {
  auto pair = make_bound_udp();
  char buf[1] = {'x'};
  (void)pair.first.sendto_blocking(buf, 0, pair.second);
}
void death_test_sendto_nonblocking_null() {
  auto pair = make_bound_udp();
  (void)pair.first.sendto_nonblocking(nullptr, 1, pair.second);
}
void death_test_sendto_nonblocking_len0() {
  auto pair = make_bound_udp();
  char buf[1] = {'x'};
  (void)pair.first.sendto_nonblocking(buf, 0, pair.second);
}
void death_test_recvfrom_blocking_null() {
  auto pair = make_bound_udp();
  (void)pair.first.recvfrom_blocking(nullptr, 1);
}
void death_test_recvfrom_blocking_len0() {
  auto pair = make_bound_udp();
  char buf[1];
  (void)pair.first.recvfrom_blocking(buf, 0);
}
void death_test_recvfrom_nonblocking_null() {
  auto pair = make_bound_udp();
  (void)pair.first.recvfrom_nonblocking(nullptr, 1);
}
void death_test_recvfrom_nonblocking_len0() {
  auto pair = make_bound_udp();
  char buf[1];
  (void)pair.first.recvfrom_nonblocking(buf, 0);
}

TEST_F(UdpSocketAssertDeathTest, SendtoBlockingNullBufAsserts) {
  EXPECT_DEATH(death_test_sendto_blocking_null(), "buf must not be null");
}

TEST_F(UdpSocketAssertDeathTest, SendtoBlockingLen0Asserts) {
  EXPECT_DEATH(death_test_sendto_blocking_len0(), "len > 0");
}

TEST_F(UdpSocketAssertDeathTest, SendtoNonblockingNullBufAsserts) {
  EXPECT_DEATH(death_test_sendto_nonblocking_null(), "buf must not be null");
}

TEST_F(UdpSocketAssertDeathTest, SendtoNonblockingLen0Asserts) {
  EXPECT_DEATH(death_test_sendto_nonblocking_len0(), "len > 0");
}

TEST_F(UdpSocketAssertDeathTest, RecvfromBlockingNullBufAsserts) {
  EXPECT_DEATH(death_test_recvfrom_blocking_null(), "buf must not be null");
}

TEST_F(UdpSocketAssertDeathTest, RecvfromBlockingLen0Asserts) {
  EXPECT_DEATH(death_test_recvfrom_blocking_len0(), "len > 0");
}

TEST_F(UdpSocketAssertDeathTest, RecvfromNonblockingNullBufAsserts) {
  EXPECT_DEATH(death_test_recvfrom_nonblocking_null(), "buf must not be null");
}

TEST_F(UdpSocketAssertDeathTest, RecvfromNonblockingLen0Asserts) {
  EXPECT_DEATH(death_test_recvfrom_nonblocking_len0(), "len > 0");
}

#endif

// ============================================================================
// fd Leak Regression
// ============================================================================

TEST(UdpSocketTest, MultipleBoundSocketsNoLeak) {
  for (int i = 0; i < 500; ++i) {
    auto [sock, addr] = make_bound_udp();
  }
}

// ============================================================================
// Multicast
// ============================================================================

// Helper: creates a UDP socket bound to INADDR_ANY on a kernel-assigned port
// with SO_REUSEADDR set. Multicast receivers must bind to INADDR_ANY (not
// loopback) so the kernel delivers multicast-addressed datagrams.
std::pair<UdpSocket, sockaddr_in> make_multicast_udp() {
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    std::abort();
  }

  UdpSocket sock(fd);
  if (!sock.set_reuseaddr(true)) {
    std::abort();
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = 0; // kernel assigns

  if (::bind(sock.get(), reinterpret_cast<const sockaddr *>(&addr),
             sizeof(addr)) != 0) {
    std::abort();
  }

  // Retrieve the kernel-assigned port.
  socklen_t addr_len = sizeof(addr);
  if (::getsockname(sock.get(), reinterpret_cast<sockaddr *>(&addr),
                    &addr_len) != 0) {
    std::abort();
  }

  return {std::move(sock), addr};
}

// Multicast group in the Organization-Local Scope range (239.0.0.0/8).
// Safe for loopback testing — never routed beyond the host.
in_addr mcast_group() {
  in_addr group{};
  inet_pton(AF_INET, "239.255.0.1", &group);
  return group;
}

in_addr loopback_iface() {
  in_addr iface{};
  iface.s_addr = htonl(INADDR_LOOPBACK);
  return iface;
}

TEST(UdpSocketMulticastTest, RoundTripOnLoopback) {
  auto [sock, addr] = make_multicast_udp();
  const in_addr group = mcast_group();
  const in_addr iface = loopback_iface();

  ASSERT_TRUE(sock.join_multicast_group(group, iface));
  ASSERT_TRUE(sock.set_multicast_loop(true));
  ASSERT_TRUE(sock.set_multicast_interface(iface));

  // Build multicast destination address using the same port we're bound to.
  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_addr = group;
  dest.sin_port = addr.sin_port;

  const char msg[] = "mcast hello";
  auto sr = sock.sendto_blocking(msg, sizeof(msg), dest);
  ASSERT_EQ(sr.status, UdpSocket::SendtoStatus::kOk);

  char buf[64]{};
  auto rr = sock.recvfrom_blocking(buf, sizeof(buf));
  ASSERT_EQ(rr.status, UdpSocket::RecvfromStatus::kOk);
  ASSERT_EQ(rr.bytes_received, static_cast<ssize_t>(sizeof(msg)));
  EXPECT_STREQ(buf, msg);
}

TEST(UdpSocketMulticastTest, JoinLeaveRejoin) {
  auto [sock, addr] = make_multicast_udp();
  const in_addr group = mcast_group();
  const in_addr iface = loopback_iface();

  ASSERT_TRUE(sock.set_multicast_loop(true));
  ASSERT_TRUE(sock.set_multicast_interface(iface));

  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_addr = group;
  dest.sin_port = addr.sin_port;

  // Phase 1: join and verify round-trip.
  ASSERT_TRUE(sock.join_multicast_group(group, iface));
  const char msg1[] = "phase1";
  auto sr1 = sock.sendto_blocking(msg1, sizeof(msg1), dest);
  ASSERT_EQ(sr1.status, UdpSocket::SendtoStatus::kOk);
  char buf[64]{};
  auto rr1 = sock.recvfrom_blocking(buf, sizeof(buf));
  ASSERT_EQ(rr1.status, UdpSocket::RecvfromStatus::kOk);
  EXPECT_STREQ(buf, msg1);

  // Phase 2: leave — send should still succeed (we're a sender, not the
  // group member for receives). But recvfrom should see nothing.
  ASSERT_TRUE(sock.leave_multicast_group(group, iface));
  const char msg2[] = "phase2";
  (void)sock.sendto_blocking(msg2, sizeof(msg2), dest);
  std::memset(buf, 0, sizeof(buf));
  auto rr2 = sock.recvfrom_nonblocking(buf, sizeof(buf));
  EXPECT_EQ(rr2.status, UdpSocket::RecvfromStatus::kWouldBlock);

  // Phase 3: rejoin — round-trip works again.
  ASSERT_TRUE(sock.join_multicast_group(group, iface));
  const char msg3[] = "phase3";
  auto sr3 = sock.sendto_blocking(msg3, sizeof(msg3), dest);
  ASSERT_EQ(sr3.status, UdpSocket::SendtoStatus::kOk);
  std::memset(buf, 0, sizeof(buf));
  auto rr3 = sock.recvfrom_blocking(buf, sizeof(buf));
  ASSERT_EQ(rr3.status, UdpSocket::RecvfromStatus::kOk);
  EXPECT_STREQ(buf, msg3);
}

TEST(UdpSocketMulticastTest, SetMulticastTtl) {
  auto sock = make_udp();

  // Default HFT TTL: 1 (LAN only).
  ASSERT_TRUE(sock.set_multicast_ttl(1));
  int val = -1;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), IPPROTO_IP, IP_MULTICAST_TTL, &val, &len),
            0);
  EXPECT_EQ(val, 1);

  // TTL 0: same-host only.
  ASSERT_TRUE(sock.set_multicast_ttl(0));
  ASSERT_EQ(getsockopt(sock.get(), IPPROTO_IP, IP_MULTICAST_TTL, &val, &len),
            0);
  EXPECT_EQ(val, 0);

  // TTL 32: crosses routers (rare in HFT).
  ASSERT_TRUE(sock.set_multicast_ttl(32));
  ASSERT_EQ(getsockopt(sock.get(), IPPROTO_IP, IP_MULTICAST_TTL, &val, &len),
            0);
  EXPECT_EQ(val, 32);
}

TEST(UdpSocketMulticastTest, SetMulticastLoopEnable) {
  auto sock = make_udp();
  ASSERT_TRUE(sock.set_multicast_loop(true));

  int val = -1;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), IPPROTO_IP, IP_MULTICAST_LOOP, &val, &len),
            0);
  EXPECT_NE(val, 0);
}

TEST(UdpSocketMulticastTest, SetMulticastLoopDisable) {
  auto sock = make_udp();
  ASSERT_TRUE(sock.set_multicast_loop(false));

  int val = -1;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), IPPROTO_IP, IP_MULTICAST_LOOP, &val, &len),
            0);
  EXPECT_EQ(val, 0);

  // NOTE: We only verify the sockopt value here — not the behavioral effect.
  // On Linux, IP_MULTICAST_LOOP=0 suppresses loopback at the IP routing layer,
  // but the loopback interface (lo) is special: it always delivers packets
  // locally regardless of this flag. A real behavioral test would require
  // a physical or virtual NIC (e.g., veth pair), which is too fragile for CI.
  // In production HFT, the flag matters on the 10GbE market data NIC.
}

TEST(UdpSocketMulticastTest, SetMulticastInterface) {
  auto sock = make_udp();
  const in_addr iface = loopback_iface();

  ASSERT_TRUE(sock.set_multicast_interface(iface));

  // Verify via getsockopt that the outgoing interface is now loopback.
  in_addr out_iface{};
  socklen_t len = sizeof(out_iface);
  ASSERT_EQ(
      getsockopt(sock.get(), IPPROTO_IP, IP_MULTICAST_IF, &out_iface, &len), 0);
  EXPECT_EQ(out_iface.s_addr, iface.s_addr);
}

TEST(UdpSocketMulticastTest, MultipleMessageBoundaries) {
  auto [sock, addr] = make_multicast_udp();
  const in_addr group = mcast_group();
  const in_addr iface = loopback_iface();

  ASSERT_TRUE(sock.join_multicast_group(group, iface));
  ASSERT_TRUE(sock.set_multicast_loop(true));
  ASSERT_TRUE(sock.set_multicast_interface(iface));

  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_addr = group;
  dest.sin_port = addr.sin_port;

  // Send 5 distinct datagrams.
  constexpr int kCount = 5;
  char send_buf[32];
  for (int i = 0; i < kCount; ++i) {
    const int n = std::snprintf(send_buf, sizeof(send_buf), "SEQ:%03d", i);
    auto sr =
        sock.sendto_blocking(send_buf, static_cast<std::size_t>(n) + 1, dest);
    ASSERT_EQ(sr.status, UdpSocket::SendtoStatus::kOk);
  }

  // Receive 5 datagrams and verify each preserves its boundary.
  for (int i = 0; i < kCount; ++i) {
    char recv_buf[32]{};
    auto rr = sock.recvfrom_blocking(recv_buf, sizeof(recv_buf));
    ASSERT_EQ(rr.status, UdpSocket::RecvfromStatus::kOk);

    char expected[32];
    std::snprintf(expected, sizeof(expected), "SEQ:%03d", i);
    EXPECT_STREQ(recv_buf, expected) << "Mismatch at datagram " << i;
  }
}

} // namespace
} // namespace mk::net
