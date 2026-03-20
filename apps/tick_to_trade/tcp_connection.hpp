/**
 * @file tcp_connection.hpp
 * @brief TCP connection lifecycle management — connect, heartbeat,
 *        reconnection, send diagnostics.
 *
 * Extracted from main.cpp to keep the event loop focused on pipeline logic.
 * Contains:
 *   - ConnectionState: tracks TCP health (heartbeat timestamps, reconnect
 *     progress, diagnostic counters).
 *   - connect_to_exchange(): cold-path TCP client socket factory.
 *   - check_send_result(): send result dispatcher with diagnostics.
 *   - schedule_reconnect(): exponential backoff with capped delay.
 *   - finalize_tcp_connect(): shared bootstrap for both immediate and async
 *     connect success paths (epoll.md §8.2).
 *   - disconnect_and_reconnect(): teardown + reconnect scheduling.
 *   - Timing and buffer size constants for heartbeat / reconnection.
 */

#pragma once

#include "net/epoll_wrapper.hpp"
#include "net/tcp_socket.hpp"
#include "sys/log/signal_logger.hpp"
#include "sys/nano_clock.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <optional>
#include <sys/socket.h>

namespace mk::app {

// -- TCP connection heartbeat and reconnection timing --

constexpr std::uint64_t kHeartbeatIntervalNs = 1'000'000'000;   // 1s
constexpr std::uint64_t kHeartbeatTimeoutNs = 3'000'000'000;    // 3s
constexpr std::uint64_t kReconnectBaseDelayNs = 1'000'000'000;  // 1s
constexpr std::uint64_t kReconnectMaxDelayNs = 10'000'000'000;  // 10s
constexpr std::uint32_t kMaxReconnectAttempts = 10;
constexpr std::uint64_t kConnectTimeoutNs = 3'000'000'000; // 3s

// TCP buffer sizes — set before connect() to influence SYN window scale.
// 256KB each: large enough to avoid backpressure on burst, small enough
// to keep memory usage reasonable per connection.
constexpr int kTcpSndBufSize = 256 * 1024;
constexpr int kTcpRcvBufSize = 256 * 1024;

// -- TCP connection state machine --

enum class TcpState : std::uint8_t { kConnected, kDisconnected, kConnecting };

/// Tracks TCP connection health and reconnection progress.
/// Declared in main(), passed by reference to run_event_loop().
struct ConnectionState {
  TcpState state{TcpState::kConnected};
  std::uint64_t last_hb_sent{0};
  std::uint64_t last_hb_recv{0};
  std::uint64_t next_reconnect_at{0};
  std::uint64_t reconnect_delay{kReconnectBaseDelayNs};
  std::uint32_t reconnect_attempts{0};
  int connecting_fd{-1};        // Non-blocking connect in progress (-1 = none)
  std::uint64_t connect_deadline{0}; // monotonic_nanos deadline for kConnecting
  // Diagnostic counters — std::atomic for cross-thread monitoring safety.
  // On x86-64, relaxed atomic store compiles to a plain MOV (zero overhead).
  std::atomic<std::uint32_t> heartbeats_sent{0};
  std::atomic<std::uint32_t> heartbeats_recv{0};
  std::atomic<std::uint32_t> reconnect_count{0};
  std::atomic<std::uint32_t> send_would_block{0}; // EAGAIN — kernel buffer full
  std::atomic<std::uint32_t> send_failures{0};     // EPIPE/ECONNRESET/fatal
};

// -- Connection lifecycle helpers --

/// Schedule a reconnect attempt with exponential backoff.
/// @param reason  Human-readable failure cause (nullptr to suppress log).
inline void schedule_reconnect(ConnectionState &conn, std::uint64_t now,
                               const char *reason = nullptr) noexcept {
  ++conn.reconnect_attempts;
  conn.next_reconnect_at = now + conn.reconnect_delay;
  conn.reconnect_delay =
      std::min(conn.reconnect_delay * 2, kReconnectMaxDelayNs);
  if (reason) {
    mk::sys::log::signal_log("[PIPELINE] Reconnect attempt ",
                             conn.reconnect_attempts, " failed: ", reason,
                             '\n');
  }
}

/// Tear down TCP connection and schedule reconnection.
/// Reusable for all disconnect triggers: recv error, send error,
/// heartbeat timeout, epoll error.
inline void disconnect_and_reconnect(mk::net::TcpSocket &tcp_sock,
                                     mk::net::EpollWrapper &epoll,
                                     ConnectionState &conn,
                                     std::size_t &tcp_rx_read,
                                     std::size_t &tcp_rx_write,
                                     const char *reason) noexcept {
  if (conn.state != TcpState::kConnected) {
    return; // Already disconnected — idempotent.
  }
  mk::sys::log::signal_log("[PIPELINE] ", reason, '\n');
  epoll.remove(tcp_sock.get());
  tcp_sock.reset();
  tcp_rx_read = 0;
  tcp_rx_write = 0;
  conn.state = TcpState::kDisconnected;
  const auto now = mk::sys::monotonic_nanos();
  conn.next_reconnect_at = now + kReconnectBaseDelayNs;
  conn.reconnect_delay = kReconnectBaseDelayNs;
  conn.reconnect_attempts = 0;
}

/// Shared bootstrap for both immediate and async connect success.
/// Factored out to prevent the two paths from diverging (epoll.md §8.2).
/// @return true on success, false if critical socket options failed
///         (caller must close and retry).
[[nodiscard]] inline bool
finalize_tcp_connect(int connected_fd, mk::net::TcpSocket &tcp_sock,
                     mk::net::EpollWrapper &epoll,
                     ConnectionState &conn) noexcept {
  tcp_sock = mk::net::TcpSocket(connected_fd);

  // TCP_NODELAY + nonblocking are critical — failure means the connection
  // is unusable (Nagle adds 40ms latency, blocking stalls the event loop).
  if (!tcp_sock.set_tcp_nodelay(true)) {
    mk::sys::log::signal_log(
        "[PIPELINE] TCP_NODELAY failed — connection unusable\n");
    tcp_sock.reset();
    return false;
  }
  if (!tcp_sock.set_nonblocking()) {
    mk::sys::log::signal_log(
        "[PIPELINE] set_nonblocking failed — connection unusable\n");
    tcp_sock.reset();
    return false;
  }

  // TCP_QUICKACK is a best-effort hint — failure is non-fatal.
  (void)tcp_sock.set_tcp_quickack(true);
  // SO_BUSY_POLL: kernel polls NIC before sleeping — ~5-20us latency reduction.
  (void)tcp_sock.set_busy_poll(50);

  epoll.add(tcp_sock.get(), EPOLLIN | EPOLLET, &conn);
  const auto ts = mk::sys::monotonic_nanos();
  conn.last_hb_sent = ts;
  conn.last_hb_recv = ts;
  conn.state = TcpState::kConnected;
  conn.connecting_fd = -1;
  conn.reconnect_delay = kReconnectBaseDelayNs;
  conn.reconnect_attempts = 0;
  conn.reconnect_count.fetch_add(1, std::memory_order_relaxed);
  mk::sys::log::signal_log("[PIPELINE] Reconnected to exchange\n");
  return true;
}

// -- TCP client socket factory (cold path — startup and reconnect) --

/// Create and connect a TCP client socket to the exchange.
/// Sets TCP_NODELAY, nonblocking, TCP_QUICKACK, SO_BUSY_POLL.
/// @return Connected TcpSocket on success, std::nullopt on failure.
[[nodiscard]] inline std::optional<net::TcpSocket>
connect_to_exchange(const char *host, std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    sys::log::signal_log("[PIPELINE] socket() failed: ", strerror(errno), '\n');
    return std::nullopt;
  }
  net::TcpSocket sock(fd);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    sys::log::signal_log("[PIPELINE] Invalid host: ", host, '\n');
    return std::nullopt;
  }

  // Set buffer sizes BEFORE connect() — affects SYN window scale negotiation.
  // Best-effort: kernel may clamp to sysctl limits
  // (net.core.wmem_max/rmem_max).
  (void)sock.set_sndbuf(kTcpSndBufSize);
  (void)sock.set_rcvbuf(kTcpRcvBufSize);

  // Blocking connect (cold path — happens once at startup).
  if (::connect(sock.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) <
      0) {
    sys::log::signal_log("[PIPELINE] connect() failed: ", strerror(errno),
                         '\n');
    return std::nullopt;
  }

  // Configure for low-latency operation.
  // TCP_NODELAY and nonblocking are critical — abort startup on failure.
  if (!sock.set_tcp_nodelay(true)) {
    sys::log::signal_log(
        "[PIPELINE] TCP_NODELAY failed — connection unusable\n");
    return std::nullopt;
  }
  if (!sock.set_nonblocking()) {
    sys::log::signal_log(
        "[PIPELINE] set_nonblocking failed — connection unusable\n");
    return std::nullopt;
  }
  // TCP_QUICKACK is a best-effort hint — failure is non-fatal.
  (void)sock.set_tcp_quickack(true);
  // SO_BUSY_POLL: kernel polls NIC driver before sleeping in epoll_wait.
  // Reduces recv wakeup latency by ~5-20us. Best-effort — requires NAPI driver.
  (void)sock.set_busy_poll(50);

  return sock;
}

// -- TCP send result diagnostics --

/// Check send result and update diagnostics.
/// @return true if send succeeded or would-block (non-fatal),
///         false if connection is dead (caller must disconnect).
[[nodiscard]] inline bool
check_send_result(net::TcpSocket::SendResult result, ConnectionState &conn,
                  const char *context) noexcept {
  using S = net::TcpSocket::SendStatus;
  switch (result.status) {
  case S::kOk:
    return true;
  case S::kWouldBlock:
    conn.send_would_block.fetch_add(1, std::memory_order_relaxed);
    sys::log::signal_log("[PIPELINE] Send would-block: ", context, '\n');
    return true; // Non-fatal — kernel buffer full momentarily.
  case S::kPeerClosed:
    conn.send_failures.fetch_add(1, std::memory_order_relaxed);
    sys::log::signal_log("[PIPELINE] Send peer-closed: ", context, '\n');
    return false;
  case S::kError:
    conn.send_failures.fetch_add(1, std::memory_order_relaxed);
    sys::log::signal_log("[PIPELINE] Send error (errno=", result.err_no,
                         "): ", context, '\n');
    return false;
  }
  std::abort(); // Unreachable — all SendStatus values handled above.
}

} // namespace mk::app
