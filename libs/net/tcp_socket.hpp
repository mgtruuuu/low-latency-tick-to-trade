/**
 * @file tcp_socket.hpp
 * @brief RAII TCP socket wrapper with send/recv utilities.
 *
 * Inherits SocketBase (abort-on-invalid, observers, socket options).
 * Adds TCP-specific operations: send, recv, shutdown, TCP_NODELAY,
 * TCP_QUICKACK.
 *
 * Design hierarchy:
 *   ScopedFd           — fd + close()
 *     +-- SocketBase   — ScopedFd + abort-on-invalid + observers + socket
 * options |     +-- TcpSocket  — SocketBase + TCP send/recv + TCP options (this
 * file) |     +-- UdpSocket  — SocketBase + UDP sendto/recvfrom + multicast
 *     +-- EpollWrapper — ScopedFd + epoll_ctl/epoll_wait
 */

#pragma once

#include "net/socket_base.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <netinet/in.h>
#include <netinet/tcp.h>

namespace mk::net {

class TcpSocket : public SocketBase {
public:
  explicit TcpSocket(int fd) noexcept : SocketBase(fd) {}

  // Move: default (delegates to SocketBase).
  TcpSocket(TcpSocket &&) noexcept = default;
  TcpSocket &operator=(TcpSocket &&) noexcept = default;

  // Non-copyable (unique fd ownership).
  TcpSocket(const TcpSocket &) = delete;
  TcpSocket &operator=(const TcpSocket &) = delete;

  // SocketBase (via ScopedFd) handles close() — destructor is defaulted.
  ~TcpSocket() = default;

  // -- TCP Socket Options --

  /** @brief Disables Nagle's algorithm (TCP_NODELAY).
   * Essential for HFT: sends packets immediately without waiting to
   * coalesce small writes. Without this, TCP may buffer small sends
   * for up to 40ms (Nagle delay). */
  [[nodiscard]] bool set_tcp_nodelay(bool enable) noexcept {
    int val = enable ? 1 : 0;
    return setsockopt(fd_.get(), IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) ==
           0;
  }

  /** @brief Disables delayed ACK (TCP_QUICKACK).
   * Linux-specific, non-sticky: the kernel resets this after each recv(),
   * so the caller must re-enable it after every recv() call on the hot path.
   * Reduces round-trip latency by ACKing received segments immediately. */
  [[nodiscard]] bool set_tcp_quickack(bool enable) noexcept {
    int val = enable ? 1 : 0;
    return setsockopt(fd_.get(), IPPROTO_TCP, TCP_QUICKACK, &val,
                      sizeof(val)) == 0;
  }

  // -- Shutdown --

  /** @brief Graceful half-close via ::shutdown().
   * @param how SHUT_RD, SHUT_WR, or SHUT_RDWR.
   * SHUT_WR sends FIN to peer — HFT orderly disconnect.
   * @return true on success, false on error (check errno). */
  [[nodiscard]] bool shutdown(int how) noexcept {
    return ::shutdown(fd_.get(), how) == 0;
  }

  // -- Send --

  /**
   * @brief Status codes for send operations.
   *
   * Mirrors RecvStatus for API symmetry. Distinguishes the three
   * fundamentally different outcomes of send():
   *   kOk         — Data sent (bytes_sent > 0).
   *   kWouldBlock — Non-blocking only: socket send buffer full.
   *                 (send() returned -1 with EAGAIN/EWOULDBLOCK)
   *                 Caller should wait for EPOLLOUT and retry.
   *   kPeerClosed — Peer closed the connection (EPIPE or ECONNRESET).
   *                 Caller must tear down and unregister from epoll.
   *   kError      — Fatal socket error (check err_no in SendResult).
   */
  enum class SendStatus { kOk, kWouldBlock, kPeerClosed, kError };

  /**
   * @brief Result of a send operation — bundles status with byte count.
   *
   * Symmetric with RecvResult. bytes_sent is only meaningful when
   * status == kOk. err_no captures errno on failure — caller decides
   * whether to log.
   */
  struct SendResult {
    SendStatus status = SendStatus::kError;
    ssize_t bytes_sent = 0;
    int err_no = 0;
  };

  /**
   * @brief Sends the entire buffer, blocking until complete or error.
   * Cold-path only — not for hot-path use. Use non-blocking variants
   * with epoll event loop for latency-sensitive paths.
   *
   * Stream sockets only (SOCK_STREAM / TCP). The retry loop handles
   * partial writes, which are normal for TCP but incorrect for
   * message-oriented sockets (SOCK_DGRAM / UDP) where send() is atomic.
   *
   * MSG_NOSIGNAL: prevents SIGPIPE if the peer has closed the connection.
   * Without it, writing to a closed peer kills the process (default
   * SIGPIPE handler). With MSG_NOSIGNAL, send() returns -1 with
   * errno == EPIPE instead, which we handle as an error return.
   *
   * Returns SendResult (not just SendStatus) so that on partial failure
   * the caller knows how many bytes were written before the error.
   * Symmetric with send_nonblocking().
   *
   * @pre buf != nullptr && len > 0 (caller's responsibility — asserted
   *      in debug builds, undefined behavior in release).
   * @return SendResult — bytes_sent == len when status == kOk.
   *         On error/peer-close, bytes_sent reflects bytes sent before failure.
   */
  [[nodiscard]] SendResult send_blocking(const char *buf,
                                         std::size_t len) noexcept {
    assert(buf && len > 0 &&
           "send_blocking: buf must not be null and len must be > 0");
    std::size_t total_sent = 0;
    while (total_sent < len) {
      // Cap per-call size at SSIZE_MAX. POSIX: if len > SSIZE_MAX, the
      // result of send() is implementation-defined. This ensures the
      // return value always fits in ssize_t without wrapping negative.
      // Cost: std::min compiles to a single compare + cmov. Branch prediction
      // is ~100% hit (len < SSIZE_MAX in practice). No measurable overhead.
      const std::size_t chunk =
          std::min(len - total_sent, static_cast<std::size_t>(SSIZE_MAX));
      const ssize_t sent =
          ::send(fd_.get(), buf + total_sent, chunk, MSG_NOSIGNAL);
      if (sent == -1) {
        if (errno == EINTR) {
          continue;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
          return {.status = SendStatus::kPeerClosed,
                  .bytes_sent = static_cast<ssize_t>(total_sent),
                  .err_no = errno};
        }
        return {.status = SendStatus::kError,
                .bytes_sent = static_cast<ssize_t>(total_sent),
                .err_no = errno};
      }
      // Defensive: send() should not return 0 for non-zero len on stream
      // sockets, but guard against infinite spin if it does.
      if (sent == 0) [[unlikely]] {
        return {.status = SendStatus::kError,
                .bytes_sent = static_cast<ssize_t>(total_sent),
                .err_no = 0};
      }
      total_sent += static_cast<std::size_t>(sent);
    }
    return {.status = SendStatus::kOk,
            .bytes_sent = static_cast<ssize_t>(total_sent),
            .err_no = 0};
  }

  /**
   * @brief Attempts a single non-blocking send.
   *
   * Does NOT loop on partial sends — the caller manages the remaining
   * buffer and waits for the next EPOLLOUT notification.
   *
   * Uses MSG_DONTWAIT to enforce per-call non-blocking semantics
   * regardless of the fd's O_NONBLOCK flag. This avoids blocking
   * if the caller forgot to call set_nonblocking().
   *
   * @pre buf != nullptr && len > 0 (caller's responsibility — asserted
   *      in debug builds, undefined behavior in release).
   * @return SendResult — status is kOk, kWouldBlock, kPeerClosed, or kError.
   *         bytes_sent is only meaningful when status == kOk.
   */
  [[nodiscard]] SendResult send_nonblocking(const char *buf,
                                            std::size_t len) noexcept {
    assert(buf && len > 0 &&
           "send_nonblocking: buf must not be null and len must be > 0");
    // Cap at SSIZE_MAX — POSIX: send() result is implementation-defined
    // if len > SSIZE_MAX. Prevents ssize_t return value wrapping negative.
    const std::size_t capped_len =
        std::min(len, static_cast<std::size_t>(SSIZE_MAX));
    ssize_t sent;
    do {
      sent = ::send(fd_.get(), buf, capped_len, MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (sent == -1 && errno == EINTR);

    if (sent == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return {
            .status = SendStatus::kWouldBlock, .bytes_sent = 0, .err_no = 0};
      }
      if (errno == EPIPE || errno == ECONNRESET) {
        return {.status = SendStatus::kPeerClosed,
                .bytes_sent = 0,
                .err_no = errno};
      }
      return {.status = SendStatus::kError, .bytes_sent = 0, .err_no = errno};
    }
    // Defensive: send() should not return 0 for non-zero len on stream
    // sockets, but guard against it to match send_blocking's behavior.
    if (sent == 0) [[unlikely]] {
      return {.status = SendStatus::kError, .bytes_sent = 0, .err_no = 0};
    }
    return {.status = SendStatus::kOk, .bytes_sent = sent, .err_no = 0};
  }

  // -- Receive --

  /**
   * @brief Status codes for receive operations.
   *
   * Distinguishes the three fundamentally different outcomes of recv():
   *   kOk         — Data received (bytes_received > 0).
   *   kPeerClosed — Orderly shutdown: peer called close() / sent FIN.
   *                 The caller must tear down the connection and
   *                 unregister from epoll. (recv() returned 0)
   *   kWouldBlock — Non-blocking only: no data available now.
   *                 (recv() returned -1 with EAGAIN/EWOULDBLOCK)
   *   kError      — Fatal socket error (check err_no in RecvResult).
   */
  enum class RecvStatus { kOk, kWouldBlock, kPeerClosed, kError };

  /**
   * @brief Result of a receive operation — bundles status with byte count.
   *
   * Returning a struct instead of an out-parameter makes the contract
   * explicit: bytes_received is only meaningful when status == kOk.
   * err_no captures errno on failure — caller decides whether to log.
   */
  struct RecvResult {
    RecvStatus status = RecvStatus::kError;
    ssize_t bytes_received = 0;
    int err_no = 0;
  };

  /**
   * @brief Blocking receive: loops until exactly `len` bytes are received,
   *        the peer closes the connection, or an error occurs.
   * Cold-path only — not for hot-path use. Use non-blocking variants
   * with epoll event loop for latency-sensitive paths.
   *
   * Stream sockets only (SOCK_STREAM / TCP). A single recv() call may
   * return fewer bytes than requested — this function loops to accumulate
   * the full `len` bytes, symmetric with send_blocking().
   *
   * If the peer closes mid-transfer, returns kPeerClosed with the number
   * of bytes received so far (which may be less than `len`).
   *
   * @param buf Output buffer (must have capacity for `len` bytes).
   * @param len Number of bytes to receive.
   * @pre buf != nullptr && len > 0 (caller's responsibility — asserted
   *      in debug builds, undefined behavior in release).
   * @return RecvResult — status is kOk, kPeerClosed, or kError (never
   * kWouldBlock). bytes_received == len when status == kOk.
   */
  [[nodiscard]] RecvResult receive_blocking(char *buf,
                                            std::size_t len) noexcept {
    assert(buf && len > 0 &&
           "receive_blocking: buf must not be null and len must be > 0");
    std::size_t total_received = 0;
    while (total_received < len) {
      // Cap per-call size at SSIZE_MAX. POSIX: if len > SSIZE_MAX, the
      // result of recv() is implementation-defined. This ensures the
      // return value always fits in ssize_t without wrapping negative.
      const std::size_t to_receive =
          std::min(len - total_received, static_cast<std::size_t>(SSIZE_MAX));
      ssize_t n;
      do {
        n = ::recv(fd_.get(), buf + total_received, to_receive, 0);
      } while (n == -1 && errno == EINTR);

      if (n < 0) {
        return {.status = RecvStatus::kError,
                .bytes_received = static_cast<ssize_t>(total_received),
                .err_no = errno};
      }
      if (n == 0) {
        // Peer closed before all requested data could be read.
        return {.status = RecvStatus::kPeerClosed,
                .bytes_received = static_cast<ssize_t>(total_received),
                .err_no = 0};
      }
      total_received += static_cast<std::size_t>(n);
    }
    return {.status = RecvStatus::kOk,
            .bytes_received = static_cast<ssize_t>(total_received),
            .err_no = 0};
  }

  /**
   * @brief Non-blocking receive: returns immediately if no data available.
   *
   * Uses MSG_DONTWAIT to enforce per-call non-blocking semantics
   * regardless of the fd's O_NONBLOCK flag. This avoids blocking
   * if the caller forgot to call set_nonblocking().
   *
   * @param buf Output buffer.
   * @param len Buffer capacity.
   * @pre buf != nullptr && len > 0 (caller's responsibility — asserted
   *      in debug builds, undefined behavior in release).
   * @return RecvResult — status is kOk, kPeerClosed, kWouldBlock, or kError.
   */
  [[nodiscard]] RecvResult receive_nonblocking(char *buf,
                                               std::size_t len) noexcept {
    assert(buf && len > 0 &&
           "receive_nonblocking: buf must not be null and len must be > 0");
    // Cap at SSIZE_MAX — POSIX: recv() result is implementation-defined
    // if len > SSIZE_MAX. Prevents ssize_t return value wrapping negative.
    const std::size_t capped_len =
        std::min(len, static_cast<std::size_t>(SSIZE_MAX));
    while (true) {
      const ssize_t n = ::recv(fd_.get(), buf, capped_len, MSG_DONTWAIT);
      if (n == -1) {
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return {.status = RecvStatus::kWouldBlock,
                  .bytes_received = 0,
                  .err_no = 0};
        }
        return {
            .status = RecvStatus::kError, .bytes_received = 0, .err_no = errno};
      }
      if (n == 0) {
        return {.status = RecvStatus::kPeerClosed,
                .bytes_received = 0,
                .err_no = 0};
      }
      return {.status = RecvStatus::kOk, .bytes_received = n, .err_no = 0};
    }
  }
};

} // namespace mk::net
