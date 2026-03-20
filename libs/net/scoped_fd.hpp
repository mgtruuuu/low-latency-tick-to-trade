/**
 * @file scoped_fd.hpp
 * @brief Minimal RAII wrapper for a raw Linux file descriptor.
 *
 * This is the lowest-level fd ownership class. It does ONE thing:
 * ensures close() is called when the fd goes out of scope.
 *
 * No socket-specific logic, no epoll logic — just fd lifecycle.
 * Higher-level wrappers (SocketBase, EpollWrapper) use this
 * internally to avoid duplicating RAII boilerplate.
 *
 * Design hierarchy:
 *   ScopedFd           — fd + close()  (this file)
 *     +-- SocketBase   — ScopedFd + abort-on-invalid + observers + socket
 * options |     +-- TcpSocket  — SocketBase + TCP send/recv + TCP options | +--
 * UdpSocket  — SocketBase + UDP sendto/recvfrom + multicast
 *     +-- EpollWrapper — ScopedFd + epoll_ctl/epoll_wait
 */

#pragma once

#include <cassert>
#include <unistd.h>
#include <utility> // std::swap

namespace mk::net {

class ScopedFd {
  int fd_ = -1;

public:
  /// Constructs from a raw fd. Does NOT abort on -1 — the caller
  /// decides whether an invalid fd is an error (unlike SocketBase
  /// which aborts, because socket creation failure is always fatal).
  explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}

  ~ScopedFd() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  // Move-only: unique ownership of the fd.
  ScopedFd(ScopedFd &&other) noexcept { swap(other); }

  // Move-only: unique ownership of the fd.
  // Move-construct temp from other (other becomes invalid), then swap
  // our fd into temp. temp's destructor closes our old fd before this
  // function returns — immediate cleanup, not deferred.
  ScopedFd &operator=(ScopedFd &&other) noexcept {
    ScopedFd tmp(std::move(other));
    swap(tmp);
    return *this;
  }

  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;

  // -- Observers --

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool is_valid() const noexcept { return fd_ >= 0; }
  explicit operator bool() const noexcept { return is_valid(); }

  // -- Mutators --

  /// Releases ownership and returns the raw fd WITHOUT closing it.
  /// After this call, the ScopedFd no longer manages the fd.
  /// The caller is responsible for closing it.
  ///
  /// Use case: passing fd ownership to another object or a C API
  /// that takes ownership (e.g., fdopen()).
  [[nodiscard]] int release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

  /// Closes the current fd (if valid) and stores new_fd.
  /// Self-reset guard: if new_fd == fd_, this is a no-op to prevent
  /// double-close. Passing your own valid fd back in is almost certainly
  /// a logic error, so we assert in debug builds. reset(-1) (close and
  /// invalidate) is always fine — only reset(same_valid_fd) triggers.
  void reset(int new_fd = -1) noexcept {
    if (new_fd == fd_) {
      assert(new_fd == -1 && "reset(fd_) with own fd is likely a bug");
      return;
    }
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = new_fd;
  }

  void swap(ScopedFd &other) noexcept { std::swap(fd_, other.fd_); }

  friend void swap(ScopedFd &a, ScopedFd &b) noexcept { a.swap(b); }
};

} // namespace mk::net
