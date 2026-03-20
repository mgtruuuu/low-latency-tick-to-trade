/**
 * @file tcp_socket_test.cpp
 * @brief Adversarial tests for TcpSocket — RAII TCP socket wrapper with
 * send/recv.
 *
 * Test strategy:
 *   - Uses socketpair(AF_UNIX, SOCK_STREAM, 0) to create connected socket
 * pairs. No network needed — everything is in-kernel, deterministic.
 *   - Verifies send/recv, blocking/nonblocking mode, peer close detection.
 *   - Death tests: abort on invalid fd, assert on len==0.
 */

#include "net/tcp_socket.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace mk::net {
namespace {

// Helper: creates a connected AF_UNIX socketpair (SOCK_STREAM).
// Aborts on failure — callers depend on valid fds.
std::pair<int, int> make_socketpair() {
  int fds[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    std::abort();
  }
  return {fds[0], fds[1]};
}

// ============================================================================
// Construction / Destruction
// ============================================================================

// Death test suite name must end with *DeathTest.
using TcpSocketDeathTest = ::testing::Test;

TEST_F(TcpSocketDeathTest, AbortOnInvalidFd) {
  EXPECT_DEATH({ const TcpSocket sock(-1); }, "FATAL.*Socket.*invalid fd");
}

TEST(TcpSocketTest, ConstructWithValidFd) {
  auto [a, b] = make_socketpair();
  ::close(b);

  const TcpSocket sock(a);
  EXPECT_TRUE(sock.is_valid());
  EXPECT_EQ(sock.get(), a);
  EXPECT_TRUE(static_cast<bool>(sock));
}

TEST(TcpSocketTest, DestructorClosesSocket) {
  auto [a, b] = make_socketpair();
  ::close(b);

  int raw_fd;
  {
    const TcpSocket sock(a);
    raw_fd = sock.get();
  }
  // After destruction, fd must be closed.
  EXPECT_EQ(fcntl(raw_fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
}

// ============================================================================
// Move Semantics
// ============================================================================

TEST(TcpSocketTest, MoveConstructTransfersOwnership) {
  auto [a, b] = make_socketpair();
  ::close(b);

  TcpSocket s1(a);
  const TcpSocket s2(std::move(s1));
  EXPECT_FALSE(s1.is_valid());  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(s2.get(), a);
}

TEST(TcpSocketTest, MoveAssignClosePrevious) {
  auto [a1, b1] = make_socketpair();
  auto [a2, b2] = make_socketpair();
  ::close(b1);
  ::close(b2);

  TcpSocket s1(a1);
  TcpSocket s2(a2);
  const int old_fd = s2.get();

  s2 = std::move(s1);
  EXPECT_FALSE(s1.is_valid());  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(s2.get(), a1);
  // Old fd (a2) must be closed.
  EXPECT_EQ(fcntl(old_fd, F_GETFD), -1);
}

TEST(TcpSocketTest, NotCopyable) {
  EXPECT_FALSE(std::is_copy_constructible_v<TcpSocket>);
  EXPECT_FALSE(std::is_copy_assignable_v<TcpSocket>);
}

TEST(TcpSocketTest, IsMovable) {
  EXPECT_TRUE(std::is_move_constructible_v<TcpSocket>);
  EXPECT_TRUE(std::is_move_assignable_v<TcpSocket>);
}

// ============================================================================
// Equality
// ============================================================================

TEST(TcpSocketTest, EqualityComparison) {
  auto [a, b] = make_socketpair();
  TcpSocket s1(a);
  const TcpSocket s2(b);
  EXPECT_FALSE(s1 == s2);

  // After move, s2 takes s1's fd — but s1 is now invalid.
  const int fd_a = s1.get();
  const TcpSocket s3(std::move(s1));
  EXPECT_EQ(s3.get(), fd_a);
}

// ============================================================================
// reset()
// ============================================================================

TEST(TcpSocketTest, ResetToInvalid) {
  auto [a, b] = make_socketpair();
  ::close(b);

  TcpSocket sock(a);
  const int old_fd = sock.get();
  sock.reset();
  EXPECT_FALSE(sock.is_valid());
  EXPECT_EQ(fcntl(old_fd, F_GETFD), -1);
}

// ============================================================================
// Blocking / Non-blocking Mode
// ============================================================================

TEST(TcpSocketTest, SetNonblockingAndBack) {
  auto [a, b] = make_socketpair();
  ::close(b);

  TcpSocket sock(a);

  // Set non-blocking.
  ASSERT_TRUE(sock.set_nonblocking());
  int flags = fcntl(sock.get(), F_GETFL, 0);
  EXPECT_NE(flags & O_NONBLOCK, 0);

  // Set back to blocking.
  ASSERT_TRUE(sock.set_blocking());
  flags = fcntl(sock.get(), F_GETFL, 0);
  EXPECT_EQ(flags & O_NONBLOCK, 0);
}

// ============================================================================
// Send / Recv — Blocking
// ============================================================================

TEST(TcpSocketTest, SendAndReceiveBlocking) {
  auto [a, b] = make_socketpair();
  TcpSocket sender(a);
  TcpSocket receiver(b);

  const char msg[] = "hello HFT";
  auto send_result = sender.send_blocking(msg, sizeof(msg));
  ASSERT_EQ(send_result.status, TcpSocket::SendStatus::kOk);
  ASSERT_EQ(send_result.bytes_sent, static_cast<ssize_t>(sizeof(msg)));

  // receive_blocking now loops until exactly sizeof(msg) bytes are received.
  char buf[64]{};
  auto result = receiver.receive_blocking(buf, sizeof(msg));
  ASSERT_EQ(result.status, TcpSocket::RecvStatus::kOk);
  ASSERT_EQ(result.bytes_received, static_cast<ssize_t>(sizeof(msg)));
  EXPECT_STREQ(buf, msg);
}

TEST(TcpSocketTest, ReceiveBlockingBlocksUntilDataArrives) {
  auto [a, b] = make_socketpair();
  TcpSocket sender(a);
  TcpSocket receiver(b);

  // Verify that receive_blocking actually blocks (waits for data) rather
  // than returning immediately. The receiver thread calls receive_blocking
  // on an empty socket. The main thread waits 50ms to give the receiver
  // time to enter the blocking recv() call, then sends data. If
  // receive_blocking returned early, the receiver would see kError or
  // kPeerClosed — not kOk with the expected data.
  const char msg[] = "unblock me";
  std::atomic<bool> recv_started{false};
  TcpSocket::RecvResult recv_result{.status = TcpSocket::RecvStatus::kError,
                                   .bytes_received = 0};
  char recv_buf[64]{};

  std::jthread receiver_thread([&] {
    recv_started.store(true, std::memory_order_release);
    recv_result = receiver.receive_blocking(recv_buf, sizeof(msg));
  });

  // Spin until the receiver thread has started. The 50ms sleep after gives
  // it time to enter the blocking recv() syscall.
  while (!recv_started.load(std::memory_order_acquire)) {
    // spin
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send data — this should unblock the receiver.
  auto send_result = sender.send_blocking(msg, sizeof(msg));
  ASSERT_EQ(send_result.status, TcpSocket::SendStatus::kOk);

  receiver_thread.join();

  EXPECT_EQ(recv_result.status, TcpSocket::RecvStatus::kOk);
  EXPECT_EQ(recv_result.bytes_received, static_cast<ssize_t>(sizeof(msg)));
  EXPECT_STREQ(recv_buf, msg);
}

TEST(TcpSocketTest, ReceiveBlockingAccumulatesPartialReads) {
  auto [a, b] = make_socketpair();
  TcpSocket sender(a);
  TcpSocket receiver(b);

  // Send two small chunks with a delay between them. receive_blocking should
  // loop and accumulate both into a single result of the requested length.
  constexpr std::size_t kTotalLen = 20;
  char send_buf[kTotalLen];
  std::memset(send_buf, 'A', 10);
  std::memset(send_buf + 10, 'B', 10);

  TcpSocket::RecvResult recv_result{.status = TcpSocket::RecvStatus::kError,
                                   .bytes_received = 0};
  char recv_buf[kTotalLen]{};

  std::jthread receiver_thread(
      [&] { recv_result = receiver.receive_blocking(recv_buf, kTotalLen); });

  // Send first half.
  auto sr1 = sender.send_blocking(send_buf, 10);
  ASSERT_EQ(sr1.status, TcpSocket::SendStatus::kOk);

  // Small delay so the receiver likely returns from the first recv() call
  // with only 10 bytes, exercising the accumulation loop.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // Send second half.
  auto sr2 = sender.send_blocking(send_buf + 10, 10);
  ASSERT_EQ(sr2.status, TcpSocket::SendStatus::kOk);

  receiver_thread.join();

  EXPECT_EQ(recv_result.status, TcpSocket::RecvStatus::kOk);
  EXPECT_EQ(recv_result.bytes_received, static_cast<ssize_t>(kTotalLen));
  EXPECT_EQ(std::memcmp(recv_buf, send_buf, kTotalLen), 0);
}

TEST(TcpSocketTest, ReceiveBlockingPeerClosesMidTransfer) {
  auto [a, b] = make_socketpair();
  TcpSocket sender(a);
  TcpSocket receiver(b);

  // Send 5 bytes, then close the sender. receive_blocking requesting 10
  // bytes should return kPeerClosed with bytes_received == 5.
  const char partial[] = "hello";
  auto sr = sender.send_blocking(partial, 5);
  ASSERT_EQ(sr.status, TcpSocket::SendStatus::kOk);

  // Close sender so receiver gets FIN after the 5 bytes.
  sender.reset();

  char buf[64]{};
  auto result = receiver.receive_blocking(buf, 10);
  EXPECT_EQ(result.status, TcpSocket::RecvStatus::kPeerClosed);
  EXPECT_EQ(result.bytes_received, 5);
  EXPECT_EQ(std::memcmp(buf, partial, 5), 0);
}

TEST(TcpSocketTest, SendLargePayloadBlocking) {
  auto [a, b] = make_socketpair();
  TcpSocket sender(a);
  TcpSocket receiver(b);

  // Shrink the send buffer so that a 64KB payload forces multiple partial
  // send() calls, exercising the retry loop in send_blocking().
  // The kernel doubles SO_SNDBUF internally and enforces a minimum (~4608),
  // but even with that, 64KB >> ~9KB guarantees multiple iterations.
  // Because the buffer is smaller than the payload, send_blocking will block
  // until the receiver drains data — so we must run the sender in a thread.
  int sndbuf = 4096;
  ASSERT_EQ(
      setsockopt(sender.get(), SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)),
      0)
      << "setsockopt SO_SNDBUF failed, errno=" << errno;

  constexpr std::size_t kSize = std::size_t{64} * 1024;
  std::array<char, kSize> send_buf{};
  for (std::size_t i = 0; i < kSize; ++i) {
    send_buf[i] = static_cast<char>(i & 0x7F);
  }

  TcpSocket::SendResult send_result{.status = TcpSocket::SendStatus::kError,
                                   .bytes_sent = 0};
  std::jthread sender_thread(
      [&] { send_result = sender.send_blocking(send_buf.data(), kSize); });

  // receive_blocking now loops for the full requested length, so a single
  // call suffices. However, send_blocking blocks until the receiver drains,
  // so we still run sender in a separate thread.
  std::array<char, kSize> recv_buf{};
  auto recv_result = receiver.receive_blocking(recv_buf.data(), kSize);

  // Close receiver to unblock sender if recv failed.
  if (recv_result.status != TcpSocket::RecvStatus::kOk) {
    receiver.reset();
  }

  sender_thread.join();
  ASSERT_EQ(recv_result.status, TcpSocket::RecvStatus::kOk);
  EXPECT_EQ(recv_result.bytes_received, static_cast<ssize_t>(kSize));
  EXPECT_EQ(send_result.status, TcpSocket::SendStatus::kOk);
  EXPECT_EQ(send_result.bytes_sent, static_cast<ssize_t>(kSize));
  EXPECT_EQ(send_buf, recv_buf);
}

TEST(TcpSocketTest, SendBlockingBlocksUnderBackPressure) {
  auto [a, b] = make_socketpair();
  TcpSocket sender(a);
  TcpSocket receiver(b);

  // 512KB payload — exceeds AF_UNIX socketpair buffer (~200KB on Linux).
  // send_blocking must block until the receiver drains data.
  constexpr std::size_t kSize = std::size_t{512} * 1024;
  std::array<char, kSize> send_buf{};
  for (std::size_t i = 0; i < kSize; ++i) {
    send_buf[i] = static_cast<char>(i & 0x7F);
  }

  TcpSocket::SendResult send_result{.status = TcpSocket::SendStatus::kError,
                                   .bytes_sent = 0};
  // std::jthread auto-joins on destruction, so early returns from
  // EXPECT_* won't leave a joinable thread (which would std::terminate).
  std::jthread sender_thread(
      [&] { send_result = sender.send_blocking(send_buf.data(), kSize); });

  // receive_blocking now loops for the full requested length. A single call
  // drains all kSize bytes. send_blocking blocks until the receiver drains,
  // so we still run sender in a separate thread.
  std::array<char, kSize> recv_buf{};
  auto recv_result = receiver.receive_blocking(recv_buf.data(), kSize);

  // Close receiver to unblock sender if recv failed.
  if (recv_result.status != TcpSocket::RecvStatus::kOk) {
    receiver.reset();
  }

  sender_thread.join();
  ASSERT_EQ(recv_result.status, TcpSocket::RecvStatus::kOk);
  EXPECT_EQ(recv_result.bytes_received, static_cast<ssize_t>(kSize));
  EXPECT_EQ(send_result.status, TcpSocket::SendStatus::kOk);
  EXPECT_EQ(send_result.bytes_sent, static_cast<ssize_t>(kSize));
  EXPECT_EQ(send_buf, recv_buf);
}

// ============================================================================
// Peer Close Detection
// ============================================================================

TEST(TcpSocketTest, ReceiveDetectsPeerClose) {
  auto [a, b] = make_socketpair();
  TcpSocket receiver(a);

  // Close the peer — receiver should detect it.
  ::close(b);

  char buf[64]{};
  auto result = receiver.receive_blocking(buf, sizeof(buf));
  EXPECT_EQ(result.status, TcpSocket::RecvStatus::kPeerClosed);
  EXPECT_EQ(result.bytes_received, 0);
}

TEST(TcpSocketTest, SendBlockingToPeerClosedReturnsPeerClosed) {
  auto [a, b] = make_socketpair();
  TcpSocket sender(a);

  // Close receiver end.
  ::close(b);

  const char msg[] = "this should fail";
  auto result = sender.send_blocking(msg, sizeof(msg));
  EXPECT_EQ(result.status, TcpSocket::SendStatus::kPeerClosed);
}

// ============================================================================
// Non-blocking Recv
// ============================================================================

TEST(TcpSocketTest, NonblockingRecvWhenNoData) {
  auto [a, b] = make_socketpair();
  TcpSocket receiver(a);
  const TcpSocket sender(b);

  ASSERT_TRUE(receiver.set_nonblocking());

  char buf[64]{};
  auto result = receiver.receive_nonblocking(buf, sizeof(buf));
  EXPECT_EQ(result.status, TcpSocket::RecvStatus::kWouldBlock);
  EXPECT_EQ(result.bytes_received, 0);
}

TEST(TcpSocketTest, NonblockingRecvWithData) {
  auto [a, b] = make_socketpair();
  TcpSocket receiver(a);
  TcpSocket sender(b);

  const char msg[] = "data";
  auto sr = sender.send_blocking(msg, sizeof(msg));
  ASSERT_EQ(sr.status, TcpSocket::SendStatus::kOk);

  ASSERT_TRUE(receiver.set_nonblocking());
  char buf[64]{};
  auto result = receiver.receive_nonblocking(buf, sizeof(buf));
  ASSERT_EQ(result.status, TcpSocket::RecvStatus::kOk);
  EXPECT_STREQ(buf, msg);
}

TEST(TcpSocketTest, NonblockingRecvPeerClose) {
  auto [a, b] = make_socketpair();
  TcpSocket receiver(a);

  ::close(b);

  ASSERT_TRUE(receiver.set_nonblocking());
  char buf[64]{};
  auto result = receiver.receive_nonblocking(buf, sizeof(buf));
  EXPECT_EQ(result.status, TcpSocket::RecvStatus::kPeerClosed);
}

// ============================================================================
// Non-blocking Send
// ============================================================================

TEST(TcpSocketTest, NonblockingSendWhenReady) {
  auto [a, b] = make_socketpair();
  TcpSocket sender(a);
  TcpSocket receiver(b);

  ASSERT_TRUE(sender.set_nonblocking());

  const char msg[] = "nb_send";
  auto send_result = sender.send_nonblocking(msg, sizeof(msg));
  ASSERT_EQ(send_result.status, TcpSocket::SendStatus::kOk);
  EXPECT_GT(send_result.bytes_sent, 0);

  // Read it back — request exactly sizeof(msg) bytes since receive_blocking
  // now loops until len bytes are received.
  char buf[64]{};
  auto result = receiver.receive_blocking(buf, sizeof(msg));
  EXPECT_EQ(result.status, TcpSocket::RecvStatus::kOk);
  EXPECT_STREQ(buf, msg);
}

TEST(TcpSocketTest, NonblockingSendBufferFullReturnsWouldBlock) {
  auto [a, b] = make_socketpair();
  TcpSocket sender(a);
  TcpSocket receiver(b);

  ASSERT_TRUE(sender.set_nonblocking());

  // Set a small send buffer to make EAGAIN deterministic across environments.
  // The kernel may round up to its minimum (e.g., 4608 on Linux), but the
  // buffer will still be small enough to fill quickly.
  int sndbuf = 4096;
  ASSERT_EQ(
      setsockopt(sender.get(), SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)),
      0)
      << "setsockopt SO_SNDBUF failed, errno=" << errno;

  // Fill the sender's socket buffer until it reports kWouldBlock.
  constexpr std::size_t kChunkSize = 8192;
  char chunk[kChunkSize];
  std::memset(chunk, 'X', kChunkSize);

  bool got_would_block = false;
  for (int i = 0; i < 1000; ++i) {
    auto send_result = sender.send_nonblocking(chunk, kChunkSize);
    if (send_result.status == TcpSocket::SendStatus::kWouldBlock) {
      got_would_block = true;
      break;
    }
    ASSERT_EQ(send_result.status, TcpSocket::SendStatus::kOk);
    EXPECT_GT(send_result.bytes_sent, 0);
  }
  ASSERT_TRUE(got_would_block)
      << "Failed to fill socket buffer after 1000 sends";

  // One more send should also return kWouldBlock.
  auto send_result = sender.send_nonblocking(chunk, kChunkSize);
  EXPECT_EQ(send_result.status, TcpSocket::SendStatus::kWouldBlock);
  EXPECT_EQ(send_result.bytes_sent, 0);

  // Drain the peer — verify sends succeed again.
  char drain[kChunkSize];
  while (true) {
    auto result = receiver.receive_nonblocking(drain, sizeof(drain));
    if (result.status == TcpSocket::RecvStatus::kWouldBlock) {
      break;
    }
    ASSERT_EQ(result.status, TcpSocket::RecvStatus::kOk);
  }

  // After draining, send should succeed again.
  send_result = sender.send_nonblocking(chunk, kChunkSize);
  ASSERT_EQ(send_result.status, TcpSocket::SendStatus::kOk);
  EXPECT_GT(send_result.bytes_sent, 0);
}

TEST(TcpSocketTest, NonblockingSendAfterPeerCloseReturnsPeerClosed) {
  auto [a, b] = make_socketpair();
  TcpSocket sender(a);

  // Close peer — send should detect peer closure (EPIPE/ECONNRESET).
  // If MSG_NOSIGNAL were missing, SIGPIPE would kill the test process,
  // so reaching the assertion also verifies SIGPIPE suppression.
  ::close(b);

  const char msg[] = "after peer close";
  auto send_result = sender.send_nonblocking(msg, sizeof(msg));
  EXPECT_EQ(send_result.status, TcpSocket::SendStatus::kPeerClosed);
  EXPECT_EQ(send_result.bytes_sent, 0);
}

// ============================================================================
// Non-blocking on blocking fd (MSG_DONTWAIT enforcement)
// ============================================================================

TEST(TcpSocketTest, NonblockingRecvOnBlockingFdReturnsWouldBlock) {
  auto [a, b] = make_socketpair();
  TcpSocket receiver(a);
  const TcpSocket sender(b);

  // Do NOT call set_nonblocking() — fd is in default blocking mode.
  // MSG_DONTWAIT must ensure receive_nonblocking returns immediately.
  char buf[64]{};
  auto result = receiver.receive_nonblocking(buf, sizeof(buf));
  EXPECT_EQ(result.status, TcpSocket::RecvStatus::kWouldBlock);
  EXPECT_EQ(result.bytes_received, 0);
}

TEST(TcpSocketTest, NonblockingSendOnBlockingFdBackPressureReturnsWouldBlock) {
  auto [a, b] = make_socketpair();
  TcpSocket sender(a);
  TcpSocket receiver(b);

  // Do NOT call set_nonblocking() — fd is in default blocking mode.
  // MSG_DONTWAIT must ensure send_nonblocking returns kWouldBlock when the
  // buffer is full, rather than blocking the caller.

  // Set a small send buffer to make back-pressure deterministic across
  // environments.
  int sndbuf = 4096;
  ASSERT_EQ(
      setsockopt(sender.get(), SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)),
      0)
      << "setsockopt SO_SNDBUF failed, errno=" << errno;

  constexpr std::size_t kChunkSize = 8192;
  char chunk[kChunkSize];
  std::memset(chunk, 'Y', kChunkSize);

  bool got_would_block = false;
  for (int i = 0; i < 1000; ++i) {
    auto send_result = sender.send_nonblocking(chunk, kChunkSize);
    if (send_result.status == TcpSocket::SendStatus::kWouldBlock) {
      got_would_block = true;
      break;
    }
    ASSERT_EQ(send_result.status, TcpSocket::SendStatus::kOk);
    EXPECT_GT(send_result.bytes_sent, 0);
  }
  ASSERT_TRUE(got_would_block)
      << "send_nonblocking on blocking fd did not return kWouldBlock — "
         "MSG_DONTWAIT may be missing";

  // Drain the peer — verify sends succeed again.
  ASSERT_TRUE(receiver.set_nonblocking());
  char drain[kChunkSize];
  while (true) {
    auto result = receiver.receive_nonblocking(drain, sizeof(drain));
    if (result.status == TcpSocket::RecvStatus::kWouldBlock) {
      break;
    }
    ASSERT_EQ(result.status, TcpSocket::RecvStatus::kOk);
  }

  // After draining, send should succeed again.
  auto send_result = sender.send_nonblocking(chunk, kChunkSize);
  ASSERT_EQ(send_result.status, TcpSocket::SendStatus::kOk);
  EXPECT_GT(send_result.bytes_sent, 0);
}

TEST(TcpSocketTest, NonblockingSendOnBlockingFdSucceeds) {
  auto [a, b] = make_socketpair();
  TcpSocket sender(a);
  TcpSocket receiver(b);

  // Do NOT call set_nonblocking() — fd is in default blocking mode.
  // MSG_DONTWAIT must ensure send_nonblocking returns immediately.
  const char msg[] = "dontwait";
  auto send_result = sender.send_nonblocking(msg, sizeof(msg));
  // Socket buffer should have space — expect success.
  ASSERT_EQ(send_result.status, TcpSocket::SendStatus::kOk);
  EXPECT_GT(send_result.bytes_sent, 0);

  // Verify data arrived — request exactly sizeof(msg) bytes since
  // receive_blocking now loops until len bytes are received.
  char buf[64]{};
  auto result = receiver.receive_blocking(buf, sizeof(msg));
  EXPECT_EQ(result.status, TcpSocket::RecvStatus::kOk);
  EXPECT_STREQ(buf, msg);
}

// ============================================================================
// Assert on len==0  (Debug only)
// ============================================================================

#ifndef NDEBUG

using TcpSocketAssertDeathTest = ::testing::Test;

TEST_F(TcpSocketAssertDeathTest, ReceiveBlockingLen0Asserts) {
  EXPECT_DEATH(
      {
        auto fds = make_socketpair();
        TcpSocket sock(fds.first);
        ::close(fds.second);
        char buf[1];
        (void)sock.receive_blocking(buf, 0);
      },
      "len > 0");
}

TEST_F(TcpSocketAssertDeathTest, ReceiveNonblockingLen0Asserts) {
  EXPECT_DEATH(
      {
        auto fds = make_socketpair();
        TcpSocket sock(fds.first);
        ::close(fds.second);
        char buf[1];
        (void)sock.receive_nonblocking(buf, 0);
      },
      "len > 0");
}

TEST_F(TcpSocketAssertDeathTest, SendBlockingLen0Asserts) {
  EXPECT_DEATH(
      {
        auto fds = make_socketpair();
        TcpSocket sock(fds.first);
        ::close(fds.second);
        char buf[1] = {'x'};
        (void)sock.send_blocking(buf, 0);
      },
      "len > 0");
}

TEST_F(TcpSocketAssertDeathTest, SendNonblockingLen0Asserts) {
  EXPECT_DEATH(
      {
        auto fds = make_socketpair();
        TcpSocket sock(fds.first);
        ::close(fds.second);
        char buf[1] = {'x'};
        (void)sock.send_nonblocking(buf, 0);
      },
      "len > 0");
}

TEST_F(TcpSocketAssertDeathTest, SendBlockingNullBufAsserts) {
  EXPECT_DEATH(
      {
        auto fds = make_socketpair();
        TcpSocket sock(fds.first);
        ::close(fds.second);
        (void)sock.send_blocking(nullptr, 1);
      },
      "buf must not be null");
}

TEST_F(TcpSocketAssertDeathTest, SendNonblockingNullBufAsserts) {
  EXPECT_DEATH(
      {
        auto fds = make_socketpair();
        TcpSocket sock(fds.first);
        ::close(fds.second);
        (void)sock.send_nonblocking(nullptr, 1);
      },
      "buf must not be null");
}

TEST_F(TcpSocketAssertDeathTest, ReceiveBlockingNullBufAsserts) {
  EXPECT_DEATH(
      {
        auto fds = make_socketpair();
        TcpSocket sock(fds.first);
        ::close(fds.second);
        (void)sock.receive_blocking(nullptr, 1);
      },
      "buf must not be null");
}

TEST_F(TcpSocketAssertDeathTest, ReceiveNonblockingNullBufAsserts) {
  EXPECT_DEATH(
      {
        auto fds = make_socketpair();
        TcpSocket sock(fds.first);
        ::close(fds.second);
        (void)sock.receive_nonblocking(nullptr, 1);
      },
      "buf must not be null");
}

#endif

// ============================================================================
// fd Leak Regression
// ============================================================================

TEST(TcpSocketTest, MultiplePairsNoLeak) {
  for (int i = 0; i < 500; ++i) {
    auto [a, b] = make_socketpair();
    const TcpSocket s1(a);
    const TcpSocket s2(b);
  }
}

// ============================================================================
// release()
// ============================================================================

TEST(TcpSocketTest, ReleaseTransfersOwnership) {
  auto [a, b] = make_socketpair();
  ::close(b);

  TcpSocket sock(a);
  const int released_fd = sock.release();

  // After release, TcpSocket no longer owns the fd.
  EXPECT_EQ(released_fd, a);
  EXPECT_EQ(sock.get(), -1);
  EXPECT_FALSE(sock.is_valid());

  // The released fd is still open — caller owns it now.
  EXPECT_NE(fcntl(released_fd, F_GETFD), -1);
  ::close(released_fd);
}

// ============================================================================
// shutdown()
// ============================================================================

TEST(TcpSocketTest, ShutdownWriteSendsFin) {
  auto [a, b] = make_socketpair();
  TcpSocket writer(a);
  TcpSocket reader(b);

  // Shut down write direction on writer — sends FIN to reader.
  ASSERT_TRUE(writer.shutdown(SHUT_WR));

  // Reader should see peer closed (FIN received).
  char buf[64]{};
  auto recv_result = reader.receive_blocking(buf, 1);
  EXPECT_EQ(recv_result.status, TcpSocket::RecvStatus::kPeerClosed);
  EXPECT_EQ(recv_result.bytes_received, 0);

  // Reverse direction is still open — reader can send to writer.
  const char msg[] = "still open";
  auto send_result = reader.send_blocking(msg, sizeof(msg));
  EXPECT_EQ(send_result.status, TcpSocket::SendStatus::kOk);

  // Writer can still receive on the open direction.
  char recv_buf[64]{};
  auto rr = writer.receive_blocking(recv_buf, sizeof(msg));
  EXPECT_EQ(rr.status, TcpSocket::RecvStatus::kOk);
  EXPECT_STREQ(recv_buf, msg);
}

// ============================================================================
// Socket Options (TCP — requires AF_INET socket)
// ============================================================================

TEST(TcpSocketTest, SetTcpNodelayOnTcpSocket) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_NE(fd, -1) << "socket() failed, errno=" << errno;

  TcpSocket sock(fd);
  ASSERT_TRUE(sock.set_tcp_nodelay(true));

  int val = 0;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), IPPROTO_TCP, TCP_NODELAY, &val, &len), 0);
  EXPECT_EQ(val, 1);

  ASSERT_TRUE(sock.set_tcp_nodelay(false));
  ASSERT_EQ(getsockopt(sock.get(), IPPROTO_TCP, TCP_NODELAY, &val, &len), 0);
  EXPECT_EQ(val, 0);
}

TEST(TcpSocketTest, SetTcpQuickackOnTcpSocket) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_NE(fd, -1) << "socket() failed, errno=" << errno;

  TcpSocket sock(fd);
  ASSERT_TRUE(sock.set_tcp_quickack(true));

  int val = 0;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), IPPROTO_TCP, TCP_QUICKACK, &val, &len), 0);
  EXPECT_EQ(val, 1);
}

TEST(TcpSocketTest, SetReuseaddrOnTcpSocket) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_NE(fd, -1) << "socket() failed, errno=" << errno;

  TcpSocket sock(fd);
  ASSERT_TRUE(sock.set_reuseaddr(true));

  int val = 0;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &val, &len), 0);
  EXPECT_EQ(val, 1);

  ASSERT_TRUE(sock.set_reuseaddr(false));
  ASSERT_EQ(getsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &val, &len), 0);
  EXPECT_EQ(val, 0);
}

TEST(TcpSocketTest, SetReuseportOnTcpSocket) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_NE(fd, -1) << "socket() failed, errno=" << errno;

  TcpSocket sock(fd);
  ASSERT_TRUE(sock.set_reuseport(true));

  int val = 0;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), SOL_SOCKET, SO_REUSEPORT, &val, &len), 0);
  EXPECT_EQ(val, 1);

  ASSERT_TRUE(sock.set_reuseport(false));
  ASSERT_EQ(getsockopt(sock.get(), SOL_SOCKET, SO_REUSEPORT, &val, &len), 0);
  EXPECT_EQ(val, 0);
}

TEST(TcpSocketTest, SetSndbufOnTcpSocket) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_NE(fd, -1) << "socket() failed, errno=" << errno;

  TcpSocket sock(fd);
  constexpr int kRequestedSize = 32768;
  ASSERT_TRUE(sock.set_sndbuf(kRequestedSize));

  // The kernel doubles the requested value, so the actual buffer size
  // should be >= the requested size.
  int val = 0;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), SOL_SOCKET, SO_SNDBUF, &val, &len), 0);
  EXPECT_GE(val, kRequestedSize);
}

TEST(TcpSocketTest, SetRcvbufOnTcpSocket) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_NE(fd, -1) << "socket() failed, errno=" << errno;

  TcpSocket sock(fd);
  constexpr int kRequestedSize = 32768;
  ASSERT_TRUE(sock.set_rcvbuf(kRequestedSize));

  int val = 0;
  socklen_t len = sizeof(val);
  ASSERT_EQ(getsockopt(sock.get(), SOL_SOCKET, SO_RCVBUF, &val, &len), 0);
  EXPECT_GE(val, kRequestedSize);
}

// TCP options fail on AF_UNIX sockets — verify graceful failure.
TEST(TcpSocketTest, TcpNodelayFailsOnUnixSocket) {
  auto [a, b] = make_socketpair();
  TcpSocket sock(a);
  ::close(b);

  // TCP_NODELAY is not valid for AF_UNIX — should return false.
  EXPECT_FALSE(sock.set_tcp_nodelay(true));
}

} // namespace
} // namespace mk::net
