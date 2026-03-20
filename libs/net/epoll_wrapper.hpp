/**
 * @file epoll_wrapper.hpp
 * @brief RAII wrapper around Linux epoll for I/O event multiplexing.
 *
 * epoll is a Linux-specific API that lets a single thread efficiently
 * monitor many file descriptors (sockets, pipes, etc.) for I/O readiness.
 *
 * Why epoll (not poll/select)?
 *   - select/poll: O(n) per call — kernel scans ALL registered fds every time.
 *   - epoll: O(1) amortized — kernel maintains an internal ready list and only
 *     returns the fds that actually have events. Scales to thousands of fds.
 *
 * Core Linux API (3 syscalls):
 *   epoll_create1(flags)             — create an epoll instance (returns an fd)
 *   epoll_ctl(epfd, op, fd, event)   — add/modify/remove a monitored fd
 *   epoll_wait(epfd, events, max, timeout_ms) — block until events arrive
 *
 * Typical HFT event loop:
 *   EpollWrapper ep;
 *   ep.add(market_data_socket, EPOLLIN);
 *   ep.add(order_gateway_socket, EPOLLIN | EPOLLOUT);
 *
 *   while (running) {
 *     int n = ep.wait(events, timeout_ms);
 *     for (int i = 0; i < n; ++i) {
 *       if (events[i].data.fd == market_data_socket) { ... }
 *     }
 *   }
 *
 * Edge-triggered (EPOLLET) vs Level-triggered (default):
 *   Level-triggered: epoll_wait returns the fd every time data is available.
 *     Simpler to use — if you don't read all data, it will fire again.
 *   Edge-triggered: epoll_wait returns the fd only when NEW data arrives.
 *     You must drain the entire buffer (read until EAGAIN), otherwise
 *     you will miss events. More efficient but harder to get right.
 *   HFT typically uses edge-triggered + non-blocking sockets for lowest
 *   latency (fewer epoll_wait wakeups).
 */

#pragma once

#include "net/scoped_fd.hpp"
#include "sys/log/signal_logger.hpp"

#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <sys/epoll.h>

namespace mk::net {

class EpollWrapper {
  ScopedFd epfd_;

public:
  // ========================================================================
  // Construction / Destruction
  // ========================================================================

  /**
   * @brief Creates an epoll instance.
   *
   * epoll_create1(EPOLL_CLOEXEC):
   *   - Creates a new epoll file descriptor.
   *   - EPOLL_CLOEXEC: automatically closes the epoll fd if the process
   *     calls exec(). Prevents fd leaks in child processes.
   *     This is a best practice — always use EPOLL_CLOEXEC unless you
   *     have a specific reason not to.
   */
  EpollWrapper() noexcept : epfd_(epoll_create1(EPOLL_CLOEXEC)) {
    if (!epfd_) {
      const int saved = errno;
      mk::sys::log::signal_log("[FATAL] epoll_create1 failed. errno: ", saved,
                               "\n");
      std::abort();
    }
  }

  // ScopedFd handles close() — destructor is defaulted.
  ~EpollWrapper() = default;

  // Move-only: delegates to ScopedFd's move, which transfers the fd integer
  // and sets the source to -1. The kernel identifies the epoll instance by fd
  // number — it doesn't care which variable holds that number. After move,
  // the source's ScopedFd is -1, so its destructor is a no-op (no
  // double-close). Post-move state: epfd_ holds -1. Any operation returns -1
  // with errno=EBADF. This is safe (no UB) but caller must not use a moved-from
  // EpollWrapper. Consistent with TcpSocket/UdpSocket, which are also move-only
  // via ScopedFd.
  EpollWrapper(EpollWrapper &&) noexcept = default;
  EpollWrapper &operator=(EpollWrapper &&) noexcept = default;

  // Non-copyable (unique fd ownership).
  EpollWrapper(const EpollWrapper &) = delete;
  EpollWrapper &operator=(const EpollWrapper &) = delete;

  // ========================================================================
  // fd Registration
  // ========================================================================

  /**
   * @brief Adds a file descriptor to the epoll interest list.
   *
   * @param fd     The file descriptor to monitor.
   * @param events Bitmask of events to watch for. Common flags:
   *   EPOLLIN    — fd is ready for reading (data available).
   *   EPOLLOUT   — fd is ready for writing (send buffer has space).
   *   EPOLLET    — edge-triggered mode (see file-level doc above).
   *   EPOLLRDHUP — peer called shutdown(SHUT_WR) (half-close). Must be
   *                explicitly set — unlike EPOLLHUP, the kernel does NOT
   *                auto-report this. Essential for detecting graceful shutdown
   *                without an extra read() call that returns 0. HFT use case:
   *                exchange sends half-close during session teardown.
   *                Interview note: EPOLLHUP = full close (both directions),
   *                EPOLLRDHUP = peer closed write direction only.
   *   EPOLLHUP   — peer closed the connection (always monitored,
   *                even if not explicitly set).
   *   EPOLLERR   — error on the fd (always monitored).
   * @return 0 on success, -1 on failure (check errno).
   *
   * Usage:
   *   ep.add(sock_fd, EPOLLIN);                         // level-triggered read
   *   ep.add(sock_fd, EPOLLIN | EPOLLRDHUP | EPOLLET);  // HFT typical
   *   ep.add(sock_fd, EPOLLIN | EPOLLOUT);               // read + write ready
   */
  int add(int fd, std::uint32_t events) noexcept {
    struct epoll_event ev {};
    ev.events = events;

    // ev.data is a union — you can store an fd, a pointer, or a u64.
    // Storing the fd is the simplest pattern: in the event loop you
    // check events[i].data.fd to identify which socket fired.
    ev.data.fd = fd;

    // EPOLL_CTL_ADD: register a new fd with the epoll instance.
    // Returns 0 on success, -1 on failure.
    return epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev);
  }

  /**
   * @brief Adds a file descriptor with a user-provided pointer as event data.
   *
   * @param fd       The file descriptor to monitor.
   * @param events   Bitmask of events to watch for (same flags as above).
   * @param user_ptr Pointer stored in epoll_event.data.ptr.
   *                 Returned in events[i].data.ptr when the fd fires.
   * @return 0 on success, -1 on failure (check errno).
   *
   * This avoids a secondary lookup (e.g., fd -> connection* hash map) on
   * the hot path. The caller stores a pointer to a connection/session
   * object directly, giving O(1) dispatch when events fire:
   *
   *   ep.add(sock_fd, EPOLLIN | EPOLLET, &my_connection);
   *   // ...
   *   auto* conn = static_cast<Connection*>(events[i].data.ptr);
   *
   * Important: when using this overload, read data.ptr (not data.fd)
   * from the returned epoll_event. They are different union members.
   *
   * Trade-off: void* gives O(1) dispatch but zero type safety — a wrong
   * static_cast is silent undefined behavior. Alternatives:
   *   - data.u64 as an index into a typed array (type-safe, same speed)
   *   - Template wrapper: add<T>(fd, events, T* ptr) with static_assert
   * Production code often uses void* for simplicity; know the trade-off.
   */
  int add(int fd, std::uint32_t events, void *user_ptr) noexcept {
    struct epoll_event ev {};
    ev.events = events;
    ev.data.ptr = user_ptr;
    return epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev);
  }

  /**
   * @brief Modifies the events monitored for an fd registered with add(fd,
   * events).
   *
   * @param fd     The file descriptor (must already be added with add(fd,
   * events)).
   * @param events New bitmask of events to watch for.
   * @return 0 on success, -1 on failure.
   *
   * Common use case: after connecting a socket, switch from monitoring
   * EPOLLOUT (connect completion) to EPOLLIN (data arrival).
   *
   * Important: use the correct modify overload to match how the fd was
   * added. This overload sets ev.data.fd — use it for fds registered via
   * add(fd, events). For fds registered via add(fd, events, ptr), use
   * the 3-arg modify(fd, events, user_ptr) instead.
   */
  int modify(int fd, std::uint32_t events) noexcept {
    struct epoll_event ev {};
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epfd_.get(), EPOLL_CTL_MOD, fd, &ev);
  }

  /**
   * @brief Modifies the events monitored for an fd registered with add(fd,
   * events, ptr).
   *
   * @param fd       The file descriptor (must already be added with add(fd,
   * events, ptr)).
   * @param events   New bitmask of events to watch for.
   * @param user_ptr Pointer stored in epoll_event.data.ptr.
   * @return 0 on success, -1 on failure.
   *
   * Important: use this overload only for fds registered via add(fd, events,
   * ptr). epoll_event.data is a union: writing data.fd overwrites data.ptr and
   * vice versa. EPOLL_CTL_MOD replaces the ENTIRE epoll_event (both events mask
   * and data), not just the events field. Using the wrong overload silently
   * corrupts the event identity — data.ptr becomes a garbage pointer or data.fd
   * becomes the integer representation of an address.
   */
  int modify(int fd, std::uint32_t events, void *user_ptr) noexcept {
    struct epoll_event ev {};
    ev.events = events;
    ev.data.ptr = user_ptr;
    return epoll_ctl(epfd_.get(), EPOLL_CTL_MOD, fd, &ev);
  }

  /**
   * @brief Removes a file descriptor from the epoll interest list.
   *
   * @param fd The file descriptor to stop monitoring.
   * @return 0 on success, -1 on failure.
   *
   * Note: closing the fd automatically removes it from epoll, but
   * explicit removal is cleaner and avoids surprises with dup'd fds.
   */
  int remove(int fd) noexcept {
    // EPOLL_CTL_DEL: unregister the fd.
    // The event pointer can be nullptr for DEL (kernel ignores it).
    return epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr);
  }

  // ========================================================================
  // Event Loop
  // ========================================================================

  /**
   * @brief Waits for events on the registered file descriptors.
   *
   * @param events     Output span — filled with events that fired.
   *                   The span's size determines the max results per call.
   *                   Using std::span couples buffer pointer and size,
   *                   eliminating buffer overrun from mismatched arguments.
   * @param timeout_ms How long to block:
   *   -1 — block indefinitely until at least one event fires.
   *    0 — return immediately (non-blocking poll).
   *   >0 — block for at most timeout_ms milliseconds.
   * @return Number of ready fds (0 on timeout), or -1 on error.
   *         On EINTR (interrupted by signal), returns -1 with errno==EINTR.
   *         The caller should retry in that case.
   *
   * Usage:
   *   std::array<epoll_event, 64> events{};
   *   int n = ep.wait(events, -1);
   *   for (int i = 0; i < n; ++i) {
   *     if (events[i].events & EPOLLIN) {
   *       // read from events[i].data.fd
   *     }
   *   }
   */
  int wait(std::span<struct epoll_event> events, int timeout_ms) noexcept {
    // Preconditions: caller passes a fixed events array (e.g.,
    // std::array<epoll_event, 64>). Empty or oversized spans are programmer
    // bugs, not runtime conditions.
    assert(!events.empty() && "maxevents must be > 0");
    assert(events.size() <= static_cast<std::size_t>(INT_MAX) &&
           "span too large for epoll_wait maxevents");
    return epoll_wait(epfd_.get(), events.data(),
                      static_cast<int>(events.size()), timeout_ms);
  }

  // ========================================================================
  // Observers
  // ========================================================================

  /** @brief Returns the underlying epoll file descriptor. */
  [[nodiscard]] int get() const noexcept { return epfd_.get(); }
};

} // namespace mk::net
