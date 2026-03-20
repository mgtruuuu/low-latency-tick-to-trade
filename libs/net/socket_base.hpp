/**
 * @file socket_base.hpp
 * @brief Non-virtual base class for socket RAII wrappers.
 *
 * Extracts the common code shared by TcpSocket and UdpSocket:
 *   - ScopedFd ownership with abort-on-invalid-fd policy
 *   - Observers (get, is_valid, operator bool, operator==)
 *   - Mutators (reset, release)
 *   - Blocking/non-blocking mode (fcntl)
 *   - Protocol-agnostic socket options (SO_REUSEADDR, SO_REUSEPORT,
 *     SO_SNDBUF, SO_RCVBUF)
 *
 * Design hierarchy:
 *   ScopedFd           — fd + close()
 *     +-- SocketBase   — ScopedFd + abort-on-invalid + observers + socket
 * options |     +-- TcpSocket  — SocketBase + TCP send/recv + TCP options | +--
 * UdpSocket  — SocketBase + UDP sendto/recvfrom + multicast
 *     +-- EpollWrapper — ScopedFd + epoll_ctl/epoll_wait
 *
 * Non-virtual inheritance = no vtable = zero runtime overhead.
 * fd_ is protected so derived classes can pass it to syscalls directly.
 */

#pragma once

#include "net/scoped_fd.hpp"
#include "sys/log/signal_logger.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/socket.h>

namespace mk::net {

class SocketBase {
protected:
  ScopedFd fd_;

public:
  /**
   * @brief Constructs a SocketBase from a raw file descriptor.
   * Aborts on fd == -1 because socket creation failure is unrecoverable.
   * @param fd The raw file descriptor to wrap.
   */
  explicit SocketBase(int fd) noexcept : fd_(fd) {
    if (!fd_) {
      mk::sys::log::signal_log(
          "[FATAL] Socket constructed with invalid fd (-1). "
          "The syscall that created this fd must have failed; "
          "check its return value before constructing a socket.\n");
      std::abort();
    }
  }

  // Move support — delegates to ScopedFd's move operations.
  SocketBase(SocketBase &&) noexcept = default;
  SocketBase &operator=(SocketBase &&) noexcept = default;

  // Non-copyable (unique fd ownership).
  SocketBase(const SocketBase &) = delete;
  SocketBase &operator=(const SocketBase &) = delete;

  // ScopedFd handles close() — destructor is defaulted.
  ~SocketBase() = default;

  // -- Observers (delegate to ScopedFd) --

  [[nodiscard]] int get() const noexcept { return fd_.get(); }
  [[nodiscard]] bool is_valid() const noexcept { return fd_.is_valid(); }
  explicit operator bool() const noexcept { return fd_.is_valid(); }

  /** @brief Compares underlying fd values (sentinel comparison only).
   * Does NOT imply ownership equivalence — two sockets can never
   * share an fd. Useful for fd-value sentinel checks (e.g., -1). */
  bool operator==(const SocketBase &other) const noexcept {
    return fd_.get() == other.fd_.get();
  }

  /** @brief Closes current fd and optionally stores a new one. */
  void reset(int new_fd = -1) noexcept { fd_.reset(new_fd); }

  /** @brief Releases fd ownership without closing.
   * After this call, get() returns -1 and the caller owns the fd.
   * Use case: transferring an accepted fd to another thread's event loop. */
  [[nodiscard]] int release() noexcept { return fd_.release(); }

  // -- Socket Configuration --

  [[nodiscard]] bool set_blocking() noexcept {
    const int flags = fcntl(fd_.get(), F_GETFL, 0);
    if (flags == -1) {
      const int saved = errno;
      mk::sys::log::signal_log("[ERROR] fcntl F_GETFL failed. errno: ", saved,
                               "\n");
      errno = saved;
      return false;
    }
    if (fcntl(fd_.get(), F_SETFL, flags & ~O_NONBLOCK) == -1) {
      const int saved = errno;
      mk::sys::log::signal_log("[ERROR] fcntl F_SETFL clear O_NONBLOCK failed."
                               " errno: ",
                               saved, "\n");
      errno = saved;
      return false;
    }
    return true;
  }

  [[nodiscard]] bool set_nonblocking() noexcept {
    const int flags = fcntl(fd_.get(), F_GETFL, 0);
    if (flags == -1) {
      const int saved = errno;
      mk::sys::log::signal_log("[ERROR] fcntl F_GETFL failed. errno: ", saved,
                               "\n");
      errno = saved;
      return false;
    }
    if (fcntl(fd_.get(), F_SETFL, flags | O_NONBLOCK) == -1) {
      const int saved = errno;
      mk::sys::log::signal_log("[ERROR] fcntl F_SETFL O_NONBLOCK failed."
                               " errno: ",
                               saved, "\n");
      errno = saved;
      return false;
    }
    return true;
  }

  // -- Socket Options --

  /** @brief Enables SO_REUSEADDR — allows bind() during TIME_WAIT.
   * Prevents "Address already in use" errors when restarting a server
   * that was recently shut down. */
  [[nodiscard]] bool set_reuseaddr(bool enable) noexcept {
    int val = enable ? 1 : 0;
    return setsockopt(fd_.get(), SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) ==
           0;
  }

  /** @brief Enables SO_REUSEPORT — multiple sockets bind to the same port.
   * The kernel distributes incoming connections across listeners (round-robin
   * or BPF-steered). Used for multi-threaded accept() without a shared
   * listen socket — each thread has its own socket on the same port. */
  [[nodiscard]] bool set_reuseport(bool enable) noexcept {
    int val = enable ? 1 : 0;
    return setsockopt(fd_.get(), SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val)) ==
           0;
  }

  /** @brief Sets the socket send buffer size (SO_SNDBUF).
   * The kernel doubles the requested value (for bookkeeping overhead).
   * Larger buffers absorb bursts; smaller buffers reduce latency by
   * making EAGAIN fire sooner, forcing the application to pace sends.
   * For TCP clients, set before connect() so the SYN advertises the
   * correct window scale factor. Post-connect calls still work but
   * cannot retroactively change the negotiated window scale. */
  [[nodiscard]] bool set_sndbuf(int size) noexcept {
    return setsockopt(fd_.get(), SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) ==
           0;
  }

  /** @brief Sets the socket receive buffer size (SO_RCVBUF).
   * The kernel doubles the requested value (for bookkeeping overhead).
   * For TCP clients, set before connect() so the SYN advertises the
   * correct window scale factor. Post-connect calls still work but
   * cannot retroactively change the negotiated window scale. */
  [[nodiscard]] bool set_rcvbuf(int size) noexcept {
    return setsockopt(fd_.get(), SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) ==
           0;
  }

  /** @brief Enables SO_BUSY_POLL — kernel polls the NIC driver for @p usec
   * microseconds before sleeping in epoll_wait/poll/select.
   * Reduces recv wakeup latency by ~5-20us on supported NICs.
   * Requires kernel 3.11+ and a NAPI-compliant driver.
   * Works with both TCP and UDP sockets.
   * For system-wide effect, also set sysctl net.core.busy_read and
   * net.core.busy_poll (typically 50). */
  [[nodiscard]] bool set_busy_poll(int usec) noexcept {
    return setsockopt(fd_.get(), SOL_SOCKET, SO_BUSY_POLL, &usec,
                      sizeof(usec)) == 0;
  }
};

} // namespace mk::net
