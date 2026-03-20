/**
 * @file epoll_wrapper_test.cpp
 * @brief Adversarial tests for EpollWrapper — RAII epoll wrapper.
 *
 * Test strategy:
 *   - Uses pipe() for readable/writable fd pairs (simpler than sockets for
 *     epoll testing — write end is always writable, read end becomes readable
 *     after a write).
 *   - Tests add/modify/remove, wait with timeout, ptr-based dispatch.
 *   - Verifies EPOLL_CLOEXEC is set.
 */

#include "net/epoll_wrapper.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mk::net {
namespace {

// Helper: RAII pipe pair that closes both ends on destruction.
struct PipePair {
  int read_fd;
  int write_fd;

  PipePair() {
    int fds[2];
    if (pipe(fds) != 0) {
      std::abort();
    }
    read_fd = fds[0];
    write_fd = fds[1];
  }

  ~PipePair() {
    if (read_fd >= 0) {
      ::close(read_fd);
    }
    if (write_fd >= 0) {
      ::close(write_fd);
    }
  }

  PipePair(const PipePair &) = delete;
  PipePair &operator=(const PipePair &) = delete;
  PipePair(PipePair &&) = delete;
  PipePair &operator=(PipePair &&) = delete;
};

// Helper: RAII socket pair. Uses AF_UNIX SOCK_STREAM — supports shutdown()
// for half-close (EPOLLRDHUP testing), unlike pipes.
struct SocketPair {
  int fd0;
  int fd1;

  SocketPair() {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
      std::abort();
    }
    fd0 = fds[0];
    fd1 = fds[1];
  }

  ~SocketPair() {
    if (fd0 >= 0) {
      ::close(fd0);
    }
    if (fd1 >= 0) {
      ::close(fd1);
    }
  }

  SocketPair(const SocketPair &) = delete;
  SocketPair &operator=(const SocketPair &) = delete;
  SocketPair(SocketPair &&) = delete;
  SocketPair &operator=(SocketPair &&) = delete;
};

// ============================================================================
// Construction
// ============================================================================

TEST(EpollWrapperTest, ConstructsSuccessfully) {
  const EpollWrapper ep;
  EXPECT_GE(ep.get(), 0);
}

TEST(EpollWrapperTest, EpollFdHasCloexec) {
  const EpollWrapper ep;
  const int flags = fcntl(ep.get(), F_GETFD);
  ASSERT_NE(flags, -1);
  EXPECT_NE(flags & FD_CLOEXEC, 0);
}

TEST(EpollWrapperTest, MoveOnlyNotCopyable) {
  EXPECT_FALSE(std::is_copy_constructible_v<EpollWrapper>);
  EXPECT_FALSE(std::is_copy_assignable_v<EpollWrapper>);
  EXPECT_TRUE(std::is_move_constructible_v<EpollWrapper>);
  EXPECT_TRUE(std::is_move_assignable_v<EpollWrapper>);
  // Consistent with ScopedSocket and ScopedFd: all are move-only RAII.
}

TEST(EpollWrapperTest, MoveConstructTransfersOwnership) {
  EpollWrapper ep1;
  const int epfd = ep1.get();
  ASSERT_GE(epfd, 0);

  // Move-construct ep2 from ep1. The epoll fd transfers.
  EpollWrapper ep2(std::move(ep1));
  EXPECT_EQ(ep2.get(), epfd);
  EXPECT_EQ(ep1.get(), -1); // NOLINT — moved-from state must be invalid

  // ep2 is fully functional — add/wait still work.
  const PipePair p;
  EXPECT_EQ(ep2.add(p.read_fd, EPOLLIN), 0);

  char c = 'x';
  ASSERT_EQ(::write(p.write_fd, &c, 1), 1);

  std::array<struct epoll_event, 4> events{};
  const int n = ep2.wait(events, 100);
  ASSERT_EQ(n, 1);
  EXPECT_NE(events[0].events & EPOLLIN, 0U);
}

TEST(EpollWrapperTest, MoveAssignTransfersOwnership) {
  EpollWrapper ep1;
  EpollWrapper ep2;
  const int epfd1 = ep1.get();

  // Move-assign ep1 into ep2. ep2's old fd is closed, ep1's fd transfers.
  ep2 = std::move(ep1);
  EXPECT_EQ(ep2.get(), epfd1);
  EXPECT_EQ(ep1.get(), -1); // NOLINT — moved-from state must be invalid

  // ep2 is fully functional.
  const PipePair p;
  EXPECT_EQ(ep2.add(p.write_fd, EPOLLOUT), 0);

  std::array<struct epoll_event, 4> events{};
  const int n = ep2.wait(events, 100);
  ASSERT_EQ(n, 1);
  EXPECT_NE(events[0].events & EPOLLOUT, 0U);
}

// ============================================================================
// add() / remove() — fd-based
// ============================================================================

TEST(EpollWrapperTest, AddAndRemoveFd) {
  EpollWrapper ep;
  const PipePair p;

  EXPECT_EQ(ep.add(p.read_fd, EPOLLIN), 0);
  EXPECT_EQ(ep.remove(p.read_fd), 0);
}

TEST(EpollWrapperTest, AddDuplicateFdFails) {
  EpollWrapper ep;
  const PipePair p;

  EXPECT_EQ(ep.add(p.read_fd, EPOLLIN), 0);
  // Adding the same fd again must fail with EEXIST.
  EXPECT_EQ(ep.add(p.read_fd, EPOLLIN), -1);
  EXPECT_EQ(errno, EEXIST);
}

TEST(EpollWrapperTest, RemoveUnregisteredFdFails) {
  EpollWrapper ep;
  const PipePair p;

  // Removing an fd that was never added must fail with ENOENT.
  EXPECT_EQ(ep.remove(p.read_fd), -1);
  EXPECT_EQ(errno, ENOENT);
}

TEST(EpollWrapperTest, AddInvalidFdFails) {
  EpollWrapper ep;

  // epoll_ctl(EPOLL_CTL_ADD) with fd -1 must fail with EBADF.
  EXPECT_EQ(ep.add(-1, EPOLLIN), -1);
  EXPECT_EQ(errno, EBADF);
}

TEST(EpollWrapperTest, ModifyInvalidFdFails) {
  EpollWrapper ep;

  // epoll_ctl(EPOLL_CTL_MOD) with fd -1 must fail with EBADF.
  EXPECT_EQ(ep.modify(-1, EPOLLIN), -1);
  EXPECT_EQ(errno, EBADF);
}

TEST(EpollWrapperTest, RemoveInvalidFdFails) {
  EpollWrapper ep;

  // epoll_ctl(EPOLL_CTL_DEL) with fd -1 must fail with EBADF.
  EXPECT_EQ(ep.remove(-1), -1);
  EXPECT_EQ(errno, EBADF);
}

// ============================================================================
// add() — ptr-based
// ============================================================================

TEST(EpollWrapperTest, AddWithUserPtr) {
  EpollWrapper ep;
  const PipePair p;

  int tag = 42;
  EXPECT_EQ(ep.add(p.read_fd, EPOLLIN, &tag), 0);

  // Write something to trigger readability.
  char c = 'x';
  ASSERT_EQ(::write(p.write_fd, &c, 1), 1);

  std::array<struct epoll_event, 4> events{};
  const int n = ep.wait(events, 100);
  ASSERT_EQ(n, 1);
  EXPECT_NE(events[0].events & EPOLLIN, 0U);
  // data.ptr must point to our tag.
  EXPECT_EQ(static_cast<int *>(events[0].data.ptr), &tag);
  EXPECT_EQ(*static_cast<int *>(events[0].data.ptr), 42);
}

// ============================================================================
// modify()
// ============================================================================

TEST(EpollWrapperTest, ModifyEvents) {
  EpollWrapper ep;
  const PipePair p;

  // Add read end monitoring EPOLLIN.
  EXPECT_EQ(ep.add(p.read_fd, EPOLLIN), 0);

  // Modify to monitor EPOLLOUT (nonsensical for read end of pipe,
  // but verifies the syscall succeeds).
  EXPECT_EQ(ep.modify(p.read_fd, EPOLLOUT), 0);
}

TEST(EpollWrapperTest, ModifyWithPtr) {
  EpollWrapper ep;
  const PipePair p;

  int tag1 = 1;
  EXPECT_EQ(ep.add(p.read_fd, EPOLLIN, &tag1), 0);

  int tag2 = 2;
  EXPECT_EQ(ep.modify(p.read_fd, EPOLLIN, &tag2), 0);

  // Trigger event and verify ptr was updated.
  char c = 'y';
  ASSERT_EQ(::write(p.write_fd, &c, 1), 1);

  std::array<struct epoll_event, 4> events{};
  const int n = ep.wait(events, 100);
  ASSERT_EQ(n, 1);
  EXPECT_EQ(static_cast<int *>(events[0].data.ptr), &tag2);
}

TEST(EpollWrapperTest, ModifyUnregisteredFdFails) {
  EpollWrapper ep;
  const PipePair p;

  EXPECT_EQ(ep.modify(p.read_fd, EPOLLIN), -1);
  EXPECT_EQ(errno, ENOENT);
}

TEST(EpollWrapperTest, ModifyLosesPointerWhenWrongOverloadUsed) {
  EpollWrapper ep;
  const PipePair p;

  // Add write_fd with a user pointer — uses add(fd, events, ptr).
  int tag = 99;
  ASSERT_EQ(ep.add(p.write_fd, EPOLLOUT, &tag), 0);

  // Call the WRONG modify overload — modify(fd, events) sets data.fd,
  // overwriting data.ptr. This is the API misuse the docs warn about.
  ASSERT_EQ(ep.modify(p.write_fd, EPOLLOUT), 0);

  // Write end of a pipe is immediately writable.
  std::array<struct epoll_event, 4> events{};
  const int n = ep.wait(events, 100);
  ASSERT_EQ(n, 1);
  EXPECT_NE(events[0].events & EPOLLOUT, 0U);
  // The user pointer is gone — data.fd now holds the fd value.
  EXPECT_EQ(events[0].data.fd, p.write_fd);
}

// ============================================================================
// wait()
// ============================================================================

#ifndef NDEBUG

using EpollWrapperDeathTest = ::testing::Test;

TEST_F(EpollWrapperDeathTest, WaitWithEmptySpanAsserts) {
  EXPECT_DEATH(
      {
        EpollWrapper ep;
        const std::span<struct epoll_event> empty_span;
        ep.wait(empty_span, 100);
      },
      "maxevents must be > 0");
}

#endif

TEST(EpollWrapperTest, WaitTimesOutWithNoEvents) {
  EpollWrapper ep;
  const PipePair p;

  ASSERT_EQ(ep.add(p.read_fd, EPOLLIN), 0);

  // No data written → should timeout immediately.
  std::array<struct epoll_event, 4> events{};
  const int n = ep.wait(events, 0); // non-blocking poll
  EXPECT_EQ(n, 0);
}

TEST(EpollWrapperTest, WaitReturnsReadableEvent) {
  EpollWrapper ep;
  const PipePair p;

  ASSERT_EQ(ep.add(p.read_fd, EPOLLIN), 0);

  // Write data to make the read end readable.
  const char msg[] = "epoll_test";
  ASSERT_EQ(::write(p.write_fd, msg, sizeof(msg)),
            static_cast<ssize_t>(sizeof(msg)));

  std::array<struct epoll_event, 4> events{};
  const int n = ep.wait(events, 100);
  ASSERT_EQ(n, 1);
  EXPECT_NE(events[0].events & EPOLLIN, 0U);
  EXPECT_EQ(events[0].data.fd, p.read_fd);

  // Drain the data.
  char buf[64]{};
  EXPECT_EQ(::read(p.read_fd, buf, sizeof(buf)),
            static_cast<ssize_t>(sizeof(msg)));
}

TEST(EpollWrapperTest, WaitMultipleFds) {
  EpollWrapper ep;
  const PipePair p1;
  const PipePair p2;

  ASSERT_EQ(ep.add(p1.read_fd, EPOLLIN), 0);
  ASSERT_EQ(ep.add(p2.read_fd, EPOLLIN), 0);

  // Write to both pipes.
  char c1 = 'a';
  char c2 = 'b';
  ASSERT_EQ(::write(p1.write_fd, &c1, 1), 1);
  ASSERT_EQ(::write(p2.write_fd, &c2, 1), 1);

  std::array<struct epoll_event, 4> events{};
  const int n = ep.wait(events, 100);
  ASSERT_EQ(n, 2);

  // Both read fds should be reported (order not guaranteed).
  bool found_p1 = false;
  bool found_p2 = false;
  for (int i = 0; i < n; ++i) {
    if (events[i].data.fd == p1.read_fd) {
      found_p1 = true;
    }
    if (events[i].data.fd == p2.read_fd) {
      found_p2 = true;
    }
  }
  EXPECT_TRUE(found_p1);
  EXPECT_TRUE(found_p2);
}

TEST(EpollWrapperTest, WaitDetectsHangup) {
  EpollWrapper ep;
  PipePair p;

  ASSERT_EQ(ep.add(p.read_fd, EPOLLIN), 0);

  // Close write end — read end gets EPOLLHUP.
  ::close(p.write_fd);
  p.write_fd = -1; // Prevent double close in PipePair destructor.

  std::array<struct epoll_event, 4> events{};
  const int n = ep.wait(events, 100);
  ASSERT_EQ(n, 1);
  // EPOLLHUP is always reported, even without explicit registration.
  EXPECT_NE(events[0].events & EPOLLHUP, 0U);
}

TEST(EpollWrapperTest, WaitDetectsErrorOnWriteEndWhenReadEndClosed) {
  EpollWrapper ep;
  PipePair p;

  // Monitor write end for EPOLLOUT.
  ASSERT_EQ(ep.add(p.write_fd, EPOLLOUT), 0);

  // Close read end — the write end becomes broken (no reader).
  // epoll(7): "This event [EPOLLERR] is also reported for the write end
  // of a pipe when the read end has been closed."
  // Writing to such a pipe would produce EPIPE / SIGPIPE.
  ::close(p.read_fd);
  p.read_fd = -1; // Prevent double close in PipePair destructor.

  std::array<struct epoll_event, 4> events{};
  const int n = ep.wait(events, 100);
  ASSERT_EQ(n, 1);
  // EPOLLERR is always reported by the kernel, even without explicit
  // registration in the events mask.
  EXPECT_NE(events[0].events & EPOLLERR, 0U);
}

TEST(EpollWrapperTest, WaitDetectsRdHupOnHalfClose) {
  EpollWrapper ep;
  const SocketPair sp;

  // Monitor fd0 for EPOLLIN | EPOLLRDHUP.
  // EPOLLRDHUP must be explicitly requested — the kernel does not
  // auto-report it (unlike EPOLLHUP/EPOLLERR).
  ASSERT_EQ(ep.add(sp.fd0, EPOLLIN | EPOLLRDHUP), 0);

  // Peer (fd1) performs a half-close: shuts down the write direction.
  // This is how an exchange signals graceful session teardown — the peer
  // can still read, but will send no more data.
  ASSERT_EQ(::shutdown(sp.fd1, SHUT_WR), 0);

  std::array<struct epoll_event, 4> events{};
  const int n = ep.wait(events, 100);
  ASSERT_EQ(n, 1);

  // EPOLLRDHUP must be set — peer closed its write direction.
  EXPECT_NE(events[0].events & EPOLLRDHUP, 0U);

  // EPOLLIN is also set — read() would return 0 (EOF) on the half-closed
  // stream, which the kernel considers a "readable" condition.
  EXPECT_NE(events[0].events & EPOLLIN, 0U);

  // Verify read() returns 0 (EOF) — confirms the half-close.
  char buf[16]{};
  EXPECT_EQ(::read(sp.fd0, buf, sizeof(buf)), 0);
}

// ============================================================================
// Write-readiness (EPOLLOUT)
// ============================================================================

TEST(EpollWrapperTest, WriteEndIsImmediatelyWritable) {
  EpollWrapper ep;
  const PipePair p;

  ASSERT_EQ(ep.add(p.write_fd, EPOLLOUT), 0);

  std::array<struct epoll_event, 4> events{};
  const int n = ep.wait(events, 100);
  ASSERT_EQ(n, 1);
  EXPECT_NE(events[0].events & EPOLLOUT, 0U);
  EXPECT_EQ(events[0].data.fd, p.write_fd);
}

// ============================================================================
// Removed fd no longer fires events
// ============================================================================

TEST(EpollWrapperTest, RemovedFdDoesNotFire) {
  EpollWrapper ep;
  const PipePair p;

  ASSERT_EQ(ep.add(p.read_fd, EPOLLIN), 0);
  ASSERT_EQ(ep.remove(p.read_fd), 0);

  // Write data — but since read_fd is removed, wait should timeout.
  char c = 'z';
  ASSERT_EQ(::write(p.write_fd, &c, 1), 1);

  std::array<struct epoll_event, 4> events{};
  const int n = ep.wait(events, 0);
  EXPECT_EQ(n, 0);
}

// ============================================================================
// Stress: many fds
// ============================================================================

TEST(EpollWrapperTest, ManyFds) {
  EpollWrapper ep;
  constexpr int kCount = 64;

  // Create kCount pipes and register read ends.
  std::array<PipePair, kCount> pipes{};
  for (int i = 0; i < kCount; ++i) {
    ASSERT_EQ(ep.add(pipes[i].read_fd, EPOLLIN), 0);
  }

  // Write to all pipes.
  for (int i = 0; i < kCount; ++i) {
    char c = static_cast<char>(i);
    ASSERT_EQ(::write(pipes[i].write_fd, &c, 1), 1);
  }

  // Wait and collect all events (may need multiple calls since
  // events buffer is smaller than kCount).
  int total = 0;
  std::array<struct epoll_event, 16> events{};
  while (total < kCount) {
    const int n = ep.wait(events, 100);
    ASSERT_GT(n, 0) << "Expected more events, got timeout after " << total;
    total += n;

    // Read all available data so level-triggered epoll won't re-report this fd.
    for (int i = 0; i < n; ++i) {
      char buf[64];
      const ssize_t r = ::read(events[i].data.fd, buf, sizeof(buf));
      ASSERT_GT(r, 0);
    }
  }
  EXPECT_EQ(total, kCount);
}

// ============================================================================
// Edge-triggered (EPOLLET)
// ============================================================================

TEST(EpollWrapperTest, EdgeTriggeredBehavior) {
  EpollWrapper ep;
  const PipePair p;

  // EPOLLET requires non-blocking fd — otherwise read() blocks instead
  // of returning EAGAIN, and the "drain until EAGAIN" pattern doesn't work.
  const int flags = fcntl(p.read_fd, F_GETFL);
  ASSERT_NE(flags, -1);
  ASSERT_EQ(fcntl(p.read_fd, F_SETFL, flags | O_NONBLOCK), 0);

  // Register read end with edge-triggered EPOLLIN.
  ASSERT_EQ(ep.add(p.read_fd, EPOLLIN | EPOLLET), 0);

  // --- Step 1: Write 4 bytes, verify epoll fires ---
  const char data[] = "abcd";
  ASSERT_EQ(::write(p.write_fd, data, 4), 4);

  std::array<struct epoll_event, 4> events{};
  int n = ep.wait(events, 100);
  ASSERT_EQ(n, 1);
  EXPECT_NE(events[0].events & EPOLLIN, 0U);

  // --- Step 2: Partial read — read only 1 byte, leave 3 in buffer ---
  char buf[16]{};
  ASSERT_EQ(::read(p.read_fd, buf, 1), 1);
  EXPECT_EQ(buf[0], 'a');

  // --- Step 3: epoll_wait must NOT re-fire (edge-triggered key behavior) ---
  // Data remains in the buffer, but no new edge transition occurred.
  n = ep.wait(events, 0); // non-blocking poll
  EXPECT_EQ(n, 0) << "EPOLLET must not re-notify after partial read";

  // --- Step 4: Write new data — triggers a new edge ---
  char extra = 'e';
  ASSERT_EQ(::write(p.write_fd, &extra, 1), 1);

  n = ep.wait(events, 100);
  ASSERT_EQ(n, 1);
  EXPECT_NE(events[0].events & EPOLLIN, 0U);

  // --- Step 5: Drain all remaining data until EAGAIN ---
  // This is the correct EPOLLET usage pattern: always drain fully.
  // Buffer should contain: 'b', 'c', 'd', 'e' (4 bytes).
  int total_read = 0;
  while (true) {
    const ssize_t r = ::read(p.read_fd, buf, sizeof(buf));
    if (r > 0) {
      total_read += static_cast<int>(r);
      continue;
    }
    // Non-blocking read returns -1 with EAGAIN when buffer is empty.
    ASSERT_EQ(r, -1);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    break;
  }
  EXPECT_EQ(total_read, 4); // 'b','c','d','e'

  // --- Step 6: After full drain, epoll_wait must not fire ---
  n = ep.wait(events, 0);
  EXPECT_EQ(n, 0);
}

TEST(EpollWrapperTest, OneShotBehavior) {
  EpollWrapper ep;
  const PipePair p;

  // --- Step 1: Add read end with EPOLLONESHOT ---
  // EPOLLONESHOT disables the fd after one event delivery.
  // The fd must be explicitly re-armed with modify() to receive more events.
  // This is used in multi-threaded servers to prevent multiple threads from
  // processing events on the same fd simultaneously.
  ASSERT_EQ(ep.add(p.read_fd, EPOLLIN | EPOLLONESHOT), 0);

  // --- Step 2: Write data, verify event fires once ---
  char c = 'a';
  ASSERT_EQ(::write(p.write_fd, &c, 1), 1);

  std::array<struct epoll_event, 4> events{};
  int n = ep.wait(events, 100);
  ASSERT_EQ(n, 1);
  EXPECT_NE(events[0].events & EPOLLIN, 0U);

  // Drain the data so it doesn't confuse subsequent checks.
  char buf[16]{};
  ASSERT_EQ(::read(p.read_fd, buf, sizeof(buf)), 1);

  // --- Step 3: Write more data — epoll_wait must NOT fire ---
  // EPOLLONESHOT has disabled the fd. Even though new data is available,
  // the kernel will not report it until we re-arm.
  c = 'b';
  ASSERT_EQ(::write(p.write_fd, &c, 1), 1);

  n = ep.wait(events, 0); // non-blocking poll
  EXPECT_EQ(n, 0) << "EPOLLONESHOT must suppress events until re-armed";

  // --- Step 4: Re-arm the fd with modify() ---
  ASSERT_EQ(ep.modify(p.read_fd, EPOLLIN | EPOLLONESHOT), 0);

  // --- Step 5: Verify event fires again after re-arm ---
  // The data written in step 3 is still in the buffer.
  n = ep.wait(events, 100);
  ASSERT_EQ(n, 1);
  EXPECT_NE(events[0].events & EPOLLIN, 0U);
}

TEST(EpollWrapperTest, EdgeTriggeredWriteReadiness) {
  EpollWrapper ep;
  const PipePair p;

  // Set write end to non-blocking — needed to detect EAGAIN when buffer full.
  const int flags = fcntl(p.write_fd, F_GETFL);
  ASSERT_NE(flags, -1);
  ASSERT_EQ(fcntl(p.write_fd, F_SETFL, flags | O_NONBLOCK), 0);

  // Explicitly set a small pipe buffer size to make the test deterministic.
  // Without this, the test relies on the system default (typically 65536
  // bytes), which can vary across kernel configurations and container
  // environments. F_SETPIPE_SZ (Linux 2.6.35+) sets the pipe capacity. The
  // kernel may round up to a page-aligned value, but the result is a known,
  // small buffer.
  constexpr int kPipeBufSize = 4096;
  const int actual_sz = fcntl(p.write_fd, F_SETPIPE_SZ, kPipeBufSize);
  ASSERT_GT(actual_sz, 0) << "F_SETPIPE_SZ failed: " << strerror(errno);

  // Verify the kernel allocated enough buffer for our write chunks.
  // On Linux, F_SETPIPE_SZ returns the actual (page-aligned) capacity,
  // which is always >= PIPE_BUF (4096). This assertion makes the test's
  // assumption about chunk size explicit.
  constexpr int kChunkSize = 1024;
  ASSERT_GE(actual_sz, kChunkSize) << "Pipe buffer too small for test logic";

  // --- Step 1: Register write end with EPOLLOUT | EPOLLET ---
  ASSERT_EQ(ep.add(p.write_fd, EPOLLOUT | EPOLLET), 0);

  // --- Step 2: Verify initial EPOLLOUT fires (pipe buffer is empty) ---
  std::array<struct epoll_event, 4> events{};
  int n = ep.wait(events, 100);
  ASSERT_EQ(n, 1);
  EXPECT_NE(events[0].events & EPOLLOUT, 0U);

  // --- Step 3: Fill the pipe buffer until write() returns EAGAIN ---
  // We write in 1K chunks until the kernel refuses (EAGAIN).
  char chunk[kChunkSize];
  std::memset(chunk, 'x', sizeof(chunk));
  int total_written = 0;
  while (true) {
    const ssize_t w = ::write(p.write_fd, chunk, sizeof(chunk));
    if (w > 0) {
      total_written += static_cast<int>(w);
      continue;
    }
    ASSERT_EQ(w, -1);
    ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK)
        << "Expected EAGAIN, got: " << strerror(errno);
    break;
  }
  ASSERT_GT(total_written, 0) << "Should have written at least some data";

  // --- Step 4: epoll_wait must NOT fire — no new edge since buffer full ---
  n = ep.wait(events, 0);
  EXPECT_EQ(n, 0) << "EPOLLET must not re-notify when buffer remains full";

  // --- Step 5: Drain some data from the read end to free buffer space ---
  // Reading creates space in the pipe buffer, which triggers a new
  // "not-full" edge transition on the write end.
  char drain[4096];
  const ssize_t r = ::read(p.read_fd, drain, sizeof(drain));
  ASSERT_GT(r, 0) << "Should be able to read from a full pipe";

  // --- Step 6: Verify new EPOLLOUT event fires after buffer drain ---
  n = ep.wait(events, 100);
  ASSERT_EQ(n, 1);
  EXPECT_NE(events[0].events & EPOLLOUT, 0U);
}

TEST(EpollWrapperTest, EdgeTriggeredHangupBehavior) {
  EpollWrapper ep;
  PipePair p;

  // EPOLLET requires non-blocking fd for the drain-until-EOF pattern.
  const int flags = fcntl(p.read_fd, F_GETFL);
  ASSERT_NE(flags, -1);
  ASSERT_EQ(fcntl(p.read_fd, F_SETFL, flags | O_NONBLOCK), 0);

  // Register read end with edge-triggered EPOLLIN.
  ASSERT_EQ(ep.add(p.read_fd, EPOLLIN | EPOLLET), 0);

  // --- Step 1: Write some data, then close the write end ---
  // This creates a realistic scenario: data in buffer + peer hangup.
  const char data[] = "hup";
  ASSERT_EQ(::write(p.write_fd, data, 3), 3);
  ::close(p.write_fd);
  p.write_fd = -1; // Prevent double close in PipePair destructor.

  // --- Step 2: epoll_wait should fire (EPOLLIN and/or EPOLLHUP) ---
  std::array<struct epoll_event, 4> events{};
  int n = ep.wait(events, 100);
  ASSERT_EQ(n, 1);
  // In EPOLLET mode, the kernel delivers one notification for the state
  // transition. EPOLLHUP is always reported when the write end closes.
  EXPECT_NE(events[0].events & EPOLLHUP, 0U);
  // Data was written before closing — EPOLLIN must also be set because
  // the pipe buffer is non-empty (read() would return data, not just EOF).
  EXPECT_NE(events[0].events & EPOLLIN, 0U);

  // --- Step 3: Drain to EOF ---
  // Correct EPOLLET hangup handling: read() until it returns 0 (EOF).
  // Unlike normal data (where drain ends at EAGAIN), a closed pipe
  // signals EOF with read() returning 0 after all buffered data is consumed.
  char buf[16]{};
  int total_read = 0;
  while (true) {
    const ssize_t r = ::read(p.read_fd, buf, sizeof(buf));
    if (r > 0) {
      total_read += static_cast<int>(r);
      continue;
    }
    // EOF: write end is closed and all buffered data has been read.
    ASSERT_EQ(r, 0) << "Expected EOF (0), got error: " << strerror(errno);
    break;
  }
  EXPECT_EQ(total_read, 3); // "hup"

  // --- Step 4: After draining to EOF, no further events should fire ---
  // In EPOLLET mode, the edge has been consumed. No new state transition
  // can occur on a pipe whose write end is closed and data is fully drained.
  n = ep.wait(events, 0);
  EXPECT_EQ(n, 0) << "No spurious re-notification after EPOLLET hangup drain";
}

} // namespace
} // namespace mk::net
