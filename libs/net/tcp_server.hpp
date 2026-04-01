/**
 * @file tcp_server.hpp
 * @brief Reusable edge-triggered epoll TCP server with zero-allocation hot
 * path.
 *
 * Extracts the production-grade patterns from framed_echo_server.cpp into a
 * parameterized, reusable class template. Application logic is injected via a
 * TcpHandler concept — all handler calls are resolved at compile time (no
 * virtual dispatch, no std::function).
 *
 * Key HFT patterns encapsulated:
 *   1. ET (edge-triggered) epoll with busy-spin (timeout=0) or blocking mode
 *   2. accept4(SOCK_NONBLOCK | SOCK_CLOEXEC) for zero-overhead accept
 *   3. fd-indexed flat array ConnSlot[kMaxConns] — zero alloc on accept/close
 *   4. data.u64 = (generation << 32) | fd for O(1) stale event rejection
 *   5. Hot/cold split: ConnSlot (64 bytes, L1-resident) vs buffer pools
 *   6. Raw ::recv()/::send() with EINTR retry on hot path
 *   7. EPOLLOUT toggling (enable on tx pending, disable after flush)
 *   8. EPOLLRDHUP tracking for graceful half-close
 *   9. EPOLL_CTL_DEL before close() (fd reuse race prevention)
 *   10. Pluggable framing via Framer concept (default: length-prefix)
 *
 * Memory layout:
 *   TcpServer is non-owning — all buffer memory is externally provided.
 *   Call required_buffer_size() to get the minimum allocation size, then
 *   allocate via MmapRegion (huge pages, NUMA), aligned_alloc, etc.
 *   The buffer is partitioned into:
 *     [ConnSlot × kMaxConns][rx: kMaxConns × kRxBufSize]
 *     [tx: kMaxConns × kTxBufSize][scratch: kTxBufSize]
 *   Buffer must be at least 64-byte aligned (cache-line, for ConnSlot).
 *
 * Usage:
 *   using Server = mk::net::TcpServer<EchoHandler>;
 *
 *   // Allocate buffer (MmapRegion for huge pages, or aligned_alloc).
 *   auto region = mk::sys::MmapRegion::allocate_anonymous(
 *       Server::required_buffer_size());
 *
 *   EchoHandler handler;
 *   auto server = std::make_unique<Server>(
 *       handler, mk::net::TcpServerConfig{.port = 7777},
 *       region->data(), region->size());
 *   server->listen();
 *   server->run();  // blocks until request_stop()
 */

#pragma once

#include "net/epoll_wrapper.hpp"
#include "net/length_prefix_codec.hpp"
#include "net/scoped_fd.hpp"
#include "sys/hardware_constants.hpp"
#include "sys/log/signal_logger.hpp"
#include "sys/thread/affinity.hpp"
#include "sys/thread/hot_path_control.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <climits>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>

namespace mk::net {

// ==============================================================================
// 1. ConnId — opaque connection identifier
// ==============================================================================
/// @brief Opaque connection identifier, equal to the fd value.
/// Valid only between on_connect() and on_disconnect() callbacks.
/// Must not be used after on_disconnect() returns.

using ConnId = int;

// ==============================================================================
// 2. Framer concept
// ==============================================================================
/// A Framer delimits messages within a TCP byte stream. The server calls
/// decode() after each recv() to extract complete messages, and encode()
/// when the handler queues a response via server.send().

template <typename F>
concept Framer =
    requires(const F &f, std::span<const std::byte> rx_buf,
             std::span<std::byte> tx_buf, std::span<const std::byte> payload) {
      { f.decode(rx_buf) } noexcept -> std::same_as<FrameDecodeResult>;
      { f.encode(tx_buf, payload) } noexcept -> std::same_as<std::size_t>;
    };

// ==============================================================================
// 3. TcpHandler concept
// ==============================================================================
/// The caller implements a type satisfying this concept and passes it as a
/// template argument to TcpServer. All methods are called on the event loop
/// thread — they must be noexcept, zero-allocation, and non-blocking.

template <typename H>
concept TcpHandler =
    requires(H &h, ConnId id, std::span<const std::byte> payload,
             std::span<std::byte> tx_space) {
      /// Called on each accepted connection. Return false to reject
      /// immediately.
      { h.on_connect(id) } noexcept -> std::same_as<bool>;

      /// Called with each decoded payload. tx_space is a writable view into the
      /// connection's tx buffer — write response bytes directly (zero-copy).
      /// @return The number of bytes written into tx_space.
      {
        h.on_data(id, payload, tx_space)
      } noexcept -> std::same_as<std::size_t>;

      /// Called when a connection closes (peer, error, or server-initiated).
      { h.on_disconnect(id) } noexcept -> std::same_as<void>;
    };

// ==============================================================================
// 4. LengthPrefixFramer — default framer wrapping length_prefix_codec.hpp
// ==============================================================================

struct LengthPrefixFramer {
  [[nodiscard]] FrameDecodeResult
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  decode(std::span<const std::byte> buf) const noexcept {
    return decode_length_prefix_frame(buf);
  }

  [[nodiscard]] std::size_t
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  encode(std::span<std::byte> tx_buf,
         std::span<const std::byte> payload) const noexcept {
    auto result = encode_length_prefix_frame(tx_buf, payload);
    return result.value_or(0);
  }
};

// ==============================================================================
// 5. RawFramer — no framing, each recv chunk is a single "message"
// ==============================================================================
// Useful for protocols where the handler defines its own message boundaries,
// or for testing where framing overhead is not needed.

struct RawFramer {
  [[nodiscard]] FrameDecodeResult
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  decode(std::span<const std::byte> buf) const noexcept {
    if (buf.empty()) {
      return {
          .status = FrameStatus::kIncomplete, .frame_size = 0, .payload = {}};
    }
    // Treat the entire buffer as one complete frame.
    return {
        .status = FrameStatus::kOk, .frame_size = buf.size(), .payload = buf};
  }

  [[nodiscard]] std::size_t
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  encode(std::span<std::byte> tx_buf,
         std::span<const std::byte> payload) const noexcept {
    if (payload.size() > tx_buf.size()) {
      return 0;
    }
    std::memcpy(tx_buf.data(), payload.data(), payload.size());
    return payload.size();
  }
};

// ==============================================================================
// 6. TcpServerConfig — cold-path configuration
// ==============================================================================

struct TcpServerConfig {
  std::uint16_t port = 0; // 0 = ephemeral (kernel assigns)
  int backlog = 128;
  bool nodelay = true;     // TCP_NODELAY on accepted sockets
  bool quickack = true;    // TCP_QUICKACK on accepted sockets (one-shot)
  bool busy_spin = true;   // epoll_wait(0) — burns CPU, lowest latency
  int pin_core = -1;       // -1 = no pinning
  bool alloc_guard = true; // set_hot_path_mode in Debug builds
};

// ==============================================================================
// 7. TcpServer
// ==============================================================================
//
// Template parameters:
//   Handler     — type satisfying TcpHandler (application logic)
//   kMaxConns   — max concurrent connections (= max fd value supported)
//   kRxBufSize  — per-connection receive buffer size (bytes)
//   kTxBufSize  — per-connection transmit buffer size (bytes)
//   FramerT     — type satisfying Framer (default: LengthPrefixFramer)
//
// IMPORTANT: This class contains large buffer arrays (default ~128 MiB
// virtual). Do NOT stack-allocate. Use std::make_unique or static storage.

template <TcpHandler Handler, int kMaxConns = 1024,
          std::size_t kRxBufSize = kDefaultMaxFrameSize,
          std::size_t kTxBufSize = kDefaultMaxFrameSize,
          Framer FramerT = LengthPrefixFramer>
class TcpServer {
  static_assert(kMaxConns > 0, "kMaxConns must be positive");
  static_assert(kRxBufSize > 0, "kRxBufSize must be positive");
  static_assert(kTxBufSize > 0, "kTxBufSize must be positive");

  // ========================================================================
  // Raw I/O helpers (hot path)
  // ========================================================================
  // Why raw syscalls instead of TcpSocket wrappers:
  //   1. TcpSocket aborts on fd=-1, incompatible with pre-allocated slots.
  //   2. Zero wrapper overhead — no RAII construction/destruction per I/O.
  //   3. Same EINTR retry + MSG_NOSIGNAL/MSG_DONTWAIT logic.

  enum class IoStatus : std::uint8_t { kOk, kWouldBlock, kPeerClosed, kError };

  struct IoResult {
    IoStatus status = IoStatus::kError;
    ssize_t bytes = 0;
  };

  static IoResult raw_recv(int fd, char *buf, std::size_t len) noexcept {
    while (true) {
      const ssize_t n = ::recv(fd, buf, len, MSG_DONTWAIT);
      if (n > 0) {
        return {.status = IoStatus::kOk, .bytes = n};
      }
      if (n == 0) {
        return {.status = IoStatus::kPeerClosed, .bytes = 0};
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return {.status = IoStatus::kWouldBlock, .bytes = 0};
      }
      return {.status = IoStatus::kError, .bytes = 0};
    }
  }

  static IoResult raw_send(int fd, const char *buf, std::size_t len) noexcept {
    while (true) {
      const ssize_t n = ::send(fd, buf, len, MSG_NOSIGNAL | MSG_DONTWAIT);
      if (n > 0) {
        return {.status = IoStatus::kOk, .bytes = n};
      }
      if (n == 0) [[unlikely]] {
        return {.status = IoStatus::kError, .bytes = 0};
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return {.status = IoStatus::kWouldBlock, .bytes = 0};
      }
      if (errno == EPIPE || errno == ECONNRESET) {
        return {.status = IoStatus::kPeerClosed, .bytes = 0};
      }
      return {.status = IoStatus::kError, .bytes = 0};
    }
  }

  // ========================================================================
  // ConnSlot — 1 cache line (64 bytes)
  // ========================================================================
  // Hot metadata for one connection. The actual rx/tx buffers live in
  // separate arrays (cold data) — they only touch cache during I/O.

  struct alignas(mk::sys::kCacheLineSize) ConnSlot {
    int fd = -1;
    std::uint32_t generation = 0;
    std::uint32_t rx_fill = 0;
    std::uint32_t tx_size = 0;
    std::uint32_t tx_sent = 0;
    bool peer_rd_closed = false;
    char *rx_buf = nullptr;
    char *tx_buf = nullptr;

    [[nodiscard]] std::uint32_t tx_pending() const noexcept {
      return tx_size - tx_sent;
    }

    [[nodiscard]] std::uint32_t rx_space() const noexcept {
      return static_cast<std::uint32_t>(kRxBufSize) - rx_fill;
    }
  };

  static_assert(sizeof(ConnSlot) == mk::sys::kCacheLineSize,
                "ConnSlot must be exactly 1 cache line (64 bytes)");

  // ========================================================================
  // Constants
  // ========================================================================

  static constexpr int kEpollBatchSize = 64;
  static constexpr std::uint64_t kListenerSentinel = UINT64_MAX;
  static constexpr std::uint32_t kFrameError = UINT32_MAX;

  // ========================================================================
  // Buffer layout constants
  // ========================================================================
  // External buffer is partitioned into four contiguous regions:
  //   [ConnSlot × kMaxConns][rx: kMaxConns × kRxBufSize]
  //   [tx: kMaxConns × kTxBufSize][scratch: kTxBufSize]
  //
  // ConnSlot is cache-line-aligned (64 bytes). The buffer base must be
  // at least 64-byte aligned (MmapRegion, aligned_alloc, posix_memalign
  // all satisfy this).

  static constexpr std::size_t kSlotsBytes =
      static_cast<std::size_t>(kMaxConns) * sizeof(ConnSlot);
  static constexpr std::size_t kRxTotalBytes =
      static_cast<std::size_t>(kMaxConns) * kRxBufSize;
  static constexpr std::size_t kTxTotalBytes =
      static_cast<std::size_t>(kMaxConns) * kTxBufSize;
  static constexpr std::size_t kScratchBytes = kTxBufSize;

  // ========================================================================
  // Data members
  // ========================================================================

  Handler &handler_;
  TcpServerConfig config_;
  [[no_unique_address]] FramerT framer_;
  EpollWrapper epoll_;
  ScopedFd listen_sock_; // initialized in listen()
  std::atomic<bool> stop_{false};
  bool started_ = false;
  std::uint16_t bound_port_ = 0; // actual port after bind

  // Pointers into externally provided buffer. Non-owning — caller manages
  // buffer lifetime (must outlive TcpServer). Follows the same external
  // storage pattern as HashMap, RingBuffer, and SPSCQueue.
  ConnSlot *slots_ = nullptr;
  char *rx_region_ = nullptr; // kMaxConns × kRxBufSize contiguous
  char *tx_region_ = nullptr; // kMaxConns × kTxBufSize contiguous
  char *scratch_ = nullptr;   // kTxBufSize

  // Epoll event batch — member (not local) so poll_once() doesn't re-init.
  std::array<struct epoll_event, kEpollBatchSize> events_{};

  // ========================================================================
  // data.u64 encoding — (generation << 32) | fd
  // ========================================================================
  // Same concept as LockFreeStack's tagged pointer ABA prevention.
  // The generation counter ensures stale events from a previous connection
  // on the same fd are O(1) discarded.

  static std::uint64_t encode_u64(const ConnSlot &s) noexcept {
    return (static_cast<std::uint64_t>(s.generation) << 32) |
           static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.fd));
  }

  // ========================================================================
  // Raw epoll_ctl helpers (for u64 encoding)
  // ========================================================================
  // EpollWrapper provides data.fd and data.ptr overloads but not data.u64.
  // For the generation-encoded pattern, we call raw epoll_ctl directly.

  int epoll_add_conn(int fd, std::uint32_t events, const ConnSlot &s) noexcept {
    struct epoll_event ev {};
    ev.events = events;
    ev.data.u64 = encode_u64(s);
    return epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &ev);
  }

  int epoll_mod_conn(int fd, std::uint32_t events, const ConnSlot &s) noexcept {
    struct epoll_event ev {};
    ev.events = events;
    ev.data.u64 = encode_u64(s);
    return epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, fd, &ev);
  }

  // ========================================================================
  // EPOLLOUT toggling
  // ========================================================================

  std::uint32_t base_events(const ConnSlot &s) const noexcept {
    std::uint32_t ev = EPOLLET;
    if (!s.peer_rd_closed) {
      ev |= EPOLLIN | EPOLLRDHUP;
    }
    return ev;
  }

  void enable_epollout(int fd, ConnSlot &s) noexcept {
    epoll_mod_conn(fd, base_events(s) | EPOLLOUT, s);
  }

  void disable_epollout(int fd, ConnSlot &s) noexcept {
    epoll_mod_conn(fd, base_events(s), s);
  }

  // ========================================================================
  // Slot lifecycle
  // ========================================================================

  void alloc_slot(int fd) noexcept {
    ConnSlot &s = slots_[fd];
    s.fd = fd;
    // generation is NOT reset — monotonic across slot lifetime.
    s.rx_fill = 0;
    s.tx_size = 0;
    s.tx_sent = 0;
    s.peer_rd_closed = false;
    s.rx_buf = rx_region_ + (static_cast<std::size_t>(fd) * kRxBufSize);
    s.tx_buf = tx_region_ + (static_cast<std::size_t>(fd) * kTxBufSize);
  }

  void release_slot(int fd) noexcept {
    ConnSlot &s = slots_[fd];
    s.fd = -1;
    ++s.generation;
  }

  // EPOLL_CTL_DEL before close() — fd reuse race prevention.
  // See Marek Majkowski "Epoll is fundamentally broken" for rationale.
  void close_connection(int fd) noexcept {
    epoll_.remove(fd);
    handler_.on_disconnect(static_cast<ConnId>(fd));
    release_slot(fd);
    ::close(fd);
  }

  // ========================================================================
  // Frame processing
  // ========================================================================
  // Decodes complete frames from rx_buf and dispatches to handler_.on_data().
  // The handler writes raw response payload into tx_space. If it returns
  // N > 0, the server encodes those N bytes through the framer and queues
  // the framed response into tx_buf.
  // Returns bytes consumed from rx_buf, or kFrameError on protocol violation.

  std::uint32_t process_frames(ConnSlot &s) noexcept {
    std::uint32_t consumed = 0;

    while (true) {
      auto remaining =
          std::as_bytes(std::span{s.rx_buf + consumed, s.rx_fill - consumed});

      auto result = framer_.decode(remaining);

      if (result.status == FrameStatus::kError) {
        return kFrameError;
      }
      if (result.status == FrameStatus::kIncomplete) {
        break;
      }

      // Provide handler with a writable scratch area for building the
      // response payload. Uses a dedicated member buffer (scratch_buf_)
      // so that response capacity is always kTxBufSize, independent of
      // how full the rx_buf is.
      auto tx_space =
          std::as_writable_bytes(std::span{scratch_, kScratchBytes});

      const std::size_t resp_len =
          handler_.on_data(static_cast<ConnId>(s.fd), result.payload, tx_space);

      // Encode the response through the framer into the tx buffer.
      if (resp_len > 0) {
        if (resp_len > tx_space.size()) [[unlikely]] {
          mk::sys::log::signal_log(
              "Handler response too large for scratch buffer, fd=", s.fd,
              " resp_len=", resp_len, " cap=", tx_space.size(), '\n');
          return kFrameError;
        }
        auto resp_payload = std::as_bytes(std::span{scratch_, resp_len});
        auto out = std::as_writable_bytes(
            std::span{s.tx_buf + s.tx_size, kTxBufSize - s.tx_size});
        const std::size_t encoded = framer_.encode(out, resp_payload);
        if (encoded == 0) [[unlikely]] {
          mk::sys::log::signal_log("Tx buffer encode failed, closing fd=", s.fd,
                                   " pending=", s.tx_pending(),
                                   " out_cap=", out.size(),
                                   " resp_len=", resp_len, '\n');
          return kFrameError;
        }
        s.tx_size += static_cast<std::uint32_t>(encoded);
      }

      consumed += static_cast<std::uint32_t>(result.frame_size);
    }

    return consumed;
  }

  void compact_rx_buf(ConnSlot &s, std::uint32_t consumed) noexcept {
    if (consumed == 0) {
      return;
    }
    const std::uint32_t remaining = s.rx_fill - consumed;
    if (remaining > 0) {
      std::memmove(s.rx_buf, s.rx_buf + consumed, remaining);
    }
    s.rx_fill = remaining;
  }

  // ========================================================================
  // ET write handler — drains tx_buf until EAGAIN or flush complete
  // ========================================================================

  [[nodiscard]] bool handle_write_et(ConnSlot &s) noexcept {
    while (s.tx_pending() > 0) {
      auto result = raw_send(s.fd, s.tx_buf + s.tx_sent, s.tx_pending());

      if (result.status == IoStatus::kWouldBlock) {
        return true;
      }
      if (result.status == IoStatus::kPeerClosed ||
          result.status == IoStatus::kError) {
        return false;
      }
      s.tx_sent += static_cast<std::uint32_t>(result.bytes);
    }

    // Fully flushed.
    s.tx_size = 0;
    s.tx_sent = 0;
    return true;
  }

  // ========================================================================
  // ET read handler — drains socket, decodes frames, dispatches to handler
  // ========================================================================

  void handle_read_et(ConnSlot &s) noexcept {
    while (true) {
      // If rx_buf is full, process and compact to make space.
      if (s.rx_space() == 0) {
        const std::uint32_t consumed = process_frames(s);
        if (consumed == kFrameError) {
          close_connection(s.fd);
          return;
        }
        compact_rx_buf(s, consumed);

        if (s.rx_space() == 0) [[unlikely]] {
          mk::sys::log::signal_log("Rx buffer stuck full for fd ", s.fd,
                                   ", closing\n");
          close_connection(s.fd);
          return;
        }
      }

      auto result = raw_recv(s.fd, s.rx_buf + s.rx_fill, s.rx_space());

      if (result.status == IoStatus::kWouldBlock) {
        break;
      }

      if (result.status == IoStatus::kPeerClosed) {
        s.peer_rd_closed = true;
        break;
      }

      if (result.status == IoStatus::kError) {
        close_connection(s.fd);
        return;
      }

      s.rx_fill += static_cast<std::uint32_t>(result.bytes);
    }

    // Process complete frames accumulated during the drain.
    const std::uint32_t consumed = process_frames(s);
    if (consumed == kFrameError) {
      close_connection(s.fd);
      return;
    }
    compact_rx_buf(s, consumed);
  }

  // ========================================================================
  // Single-event dispatcher — shared by poll_once() and run()
  // ========================================================================

  void dispatch_event(const struct epoll_event &ev) noexcept {
    const std::uint64_t u64 = ev.data.u64;
    const std::uint32_t flags = ev.events;

    // --- Listener ---
    if (u64 == kListenerSentinel) {
      handle_accept();
      return;
    }

    // --- Decode fd + generation, stale event defense ---
    auto slot_idx = static_cast<int>(u64 & 0xFFFFFFFF);
    auto gen = static_cast<std::uint32_t>(u64 >> 32);

    if (slot_idx < 0 || slot_idx >= kMaxConns || slots_[slot_idx].fd == -1 ||
        slots_[slot_idx].generation != gen) {
      return;
    }

    ConnSlot &s = slots_[slot_idx];

    // --- EPOLLIN (read) ---
    // Process before EPOLLRDHUP — data may arrive with the FIN.
    if (flags & EPOLLIN) {
      handle_read_et(s);
      if (s.fd == -1) {
        return;
      }
    }

    // --- EPOLLRDHUP (peer half-close) ---
    if (flags & EPOLLRDHUP) {
      s.peer_rd_closed = true;
    }

    // --- EPOLLHUP | EPOLLERR (fatal) ---
    if (flags & (EPOLLHUP | EPOLLERR)) {
      close_connection(s.fd);
      return;
    }

    // --- EPOLLOUT (write ready) ---
    if (flags & EPOLLOUT) {
      if (!handle_write_et(s)) {
        close_connection(s.fd);
        return;
      }
      if (s.tx_pending() == 0) {
        if (s.peer_rd_closed) {
          close_connection(s.fd);
          return;
        }
        disable_epollout(s.fd, s);
      }
    }

    // --- Enable EPOLLOUT if tx pending ---
    if (s.fd != -1 && s.tx_pending() > 0) {
      enable_epollout(s.fd, s);
    }

    // --- peer_rd_closed && tx empty → close ---
    if (s.fd != -1 && s.peer_rd_closed && s.tx_pending() == 0) {
      close_connection(s.fd);
    }
  }

  // ========================================================================
  // Accept handler — ET drain loop
  // ========================================================================

  void handle_accept() noexcept {
    while (true) {
      struct sockaddr_in clnt_adr {};
      socklen_t adr_sz = sizeof(clnt_adr);

      // accept4: atomically sets NONBLOCK + CLOEXEC (one syscall).
      const int fd = accept4(listen_sock_.get(),
                             reinterpret_cast<struct sockaddr *>(&clnt_adr),
                             &adr_sz, SOCK_NONBLOCK | SOCK_CLOEXEC);

      if (fd == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        if (errno == EINTR || errno == ECONNABORTED || errno == ENETDOWN ||
            errno == EPROTO || errno == ENOPROTOOPT || errno == EHOSTDOWN ||
            errno == ENONET || errno == EHOSTUNREACH || errno == EOPNOTSUPP ||
            errno == ENETUNREACH) {
          continue; // transient — keep draining in ET mode
        }
        if (errno == EMFILE || errno == ENFILE) {
          mk::sys::log::signal_log("accept4() EMFILE/ENFILE — fd exhaustion\n");
          break;
        }
        break;
      }

      if (fd >= kMaxConns) {
        mk::sys::log::signal_log("fd ", fd, " >= kMaxConns (", kMaxConns,
                                 "), rejecting\n");
        ::close(fd);
        continue;
      }

      // TCP_NODELAY — disable Nagle's algorithm.
      if (config_.nodelay) {
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      }

      // TCP_QUICKACK — disable delayed ACK for the first ACK.
      // One-shot: kernel reverts to delayed ACK after sending.
      // Re-setting after every recv() is possible but adds a syscall
      // per read — accept-time is a good default.
      if (config_.quickack) {
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
      }

      alloc_slot(fd);
      ConnSlot const &s = slots_[fd];

      // Let handler decide whether to accept this connection.
      if (!handler_.on_connect(static_cast<ConnId>(fd))) {
        release_slot(fd);
        ::close(fd);
        continue;
      }

      if (epoll_add_conn(fd, EPOLLIN | EPOLLRDHUP | EPOLLET, s) == -1) {
        mk::sys::log::signal_log("epoll_ctl add fd=", fd,
                                 " failed errno=", errno, '\n');
        handler_.on_disconnect(static_cast<ConnId>(fd));
        release_slot(fd);
        ::close(fd);
        continue;
      }
    }
  }

public:
  // ========================================================================
  // Static helpers — external buffer sizing
  // ========================================================================

  /// @brief Returns the minimum buffer size (bytes) needed for the given
  /// template parameters. Caller allocates (MmapRegion, aligned_alloc, etc.)
  /// and passes to the constructor. Buffer must be at least 64-byte aligned.
  static constexpr std::size_t required_buffer_size() noexcept {
    return kSlotsBytes + kRxTotalBytes + kTxTotalBytes + kScratchBytes;
  }

  // ========================================================================
  // Construction
  // ========================================================================

  /// @brief Constructs TcpServer over an externally provided buffer.
  /// @param handler Application handler (must outlive TcpServer).
  /// @param config  Cold-path configuration.
  /// @param buf     External memory region, at least required_buffer_size()
  ///                bytes, 64-byte aligned. TcpServer does NOT own this
  ///                memory — caller manages its lifetime.
  /// @param buf_size Size of the external buffer in bytes.
  /// @param framer  Framing strategy (default: LengthPrefixFramer).
  ///
  /// Aborts if buf is null, misaligned, or too small. This is a startup-time
  /// call — failure is unrecoverable (same pattern as ObjectPool, HashMap).
  explicit TcpServer(Handler &handler, TcpServerConfig config, void *buf,
                     std::size_t buf_size, FramerT framer = {}) noexcept
      : handler_(handler), config_(config), framer_(framer) {
    if (buf == nullptr ||
        (reinterpret_cast<std::uintptr_t>(buf) % alignof(ConnSlot)) != 0 ||
        buf_size < required_buffer_size()) {
      std::abort();
    }

    // Partition the buffer: [slots][rx][tx][scratch]
    auto *base = static_cast<char *>(buf);
    slots_ = reinterpret_cast<ConnSlot *>(base);
    rx_region_ = base + kSlotsBytes;
    tx_region_ = rx_region_ + kRxTotalBytes;
    scratch_ = tx_region_ + kTxTotalBytes;

    // Zero-initialize ConnSlots (fd=-1, generation=0, etc.).
    for (int i = 0; i < kMaxConns; ++i) {
      std::construct_at(&slots_[i]);
    }
  }

  // Non-copyable, non-movable (references external buffer + handler).
  TcpServer(const TcpServer &) = delete;
  TcpServer &operator=(const TcpServer &) = delete;
  TcpServer(TcpServer &&) = delete;
  TcpServer &operator=(TcpServer &&) = delete;

  ~TcpServer() = default;

  // ========================================================================
  // Setup — cold path
  // ========================================================================

  /// @brief Creates the listener socket, binds, and starts listening.
  /// Must be called before run().
  /// @return true on success, false on failure.
  [[nodiscard]] bool listen() noexcept {
    const int raw_fd =
        ::socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (raw_fd == -1) {
      mk::sys::log::signal_log("socket() failed errno=", errno, '\n');
      return false;
    }

    // Transfer ownership via ScopedFd (tolerates -1, but we just checked).
    listen_sock_.reset(raw_fd);

    int one = 1;
    if (::setsockopt(listen_sock_.get(), SOL_SOCKET, SO_REUSEADDR, &one,
                     sizeof(one)) == -1) {
      mk::sys::log::signal_log("SO_REUSEADDR failed errno=", errno, '\n');
      return false;
    }

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(config_.port);

    if (::bind(listen_sock_.get(), reinterpret_cast<struct sockaddr *>(&addr),
               sizeof(addr)) == -1) {
      mk::sys::log::signal_log("bind() failed errno=", errno, '\n');
      return false;
    }

    // Read back the actual port (useful when config_.port == 0).
    struct sockaddr_in bound {};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(listen_sock_.get(),
                      reinterpret_cast<struct sockaddr *>(&bound),
                      &bound_len) == 0) {
      bound_port_ = ntohs(bound.sin_port);
    }

    if (::listen(listen_sock_.get(), config_.backlog) == -1) {
      mk::sys::log::signal_log("listen() failed errno=", errno, '\n');
      return false;
    }

    return true;
  }

  // ========================================================================
  // Event loop
  // ========================================================================

  /// @brief One-time setup: registers listener with epoll, optionally pins
  /// core and enables hot-path allocation guard. Must be called after listen(),
  /// before poll_once() or run().
  /// @return true on success.
  [[nodiscard]] bool start() noexcept {
    assert(listen_sock_.is_valid());

    // Optional core pinning.
    if (config_.pin_core >= 0) {
      mk::sys::thread::pin_current_thread(config_.pin_core);
    }

    // Register listener with sentinel u64 — UINT64_MAX can never match
    // any valid (generation << 32 | fd) encoding.
    {
      struct epoll_event ev {};
      ev.events = EPOLLIN | EPOLLET;
      ev.data.u64 = kListenerSentinel;
      if (epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, listen_sock_.get(), &ev) ==
          -1) {
        mk::sys::log::signal_log("epoll_ctl listener failed errno=", errno,
                                 '\n');
        return false;
      }
    }

    // Hot-path allocation guard (Debug only, no-op in Release).
    if (config_.alloc_guard) {
      mk::sys::thread::set_hot_path_mode(true);
    }

    started_ = true;
    return true;
  }

  /// @brief Process one batch of epoll events. Non-blocking or blocking
  /// depending on timeout_ms.
  /// Use this for composite event loops (e.g., TCP server + UDP timer).
  /// Requires start() to have been called.
  /// @param timeout_ms epoll_wait timeout: 0 = non-blocking, -1 = block,
  ///        >0 = block up to N ms.
  /// @return Number of events dispatched, or -1 on fatal error.
  int poll_once(int timeout_ms = 0) noexcept {
    assert(started_);

    const int n = epoll_.wait(events_, timeout_ms);
    if (n == -1) {
      if (errno == EINTR) {
        return 0;
      }
      mk::sys::log::signal_log("epoll_wait() error errno=", errno, '\n');
      return -1;
    }
    if (n == 0) {
      return 0;
    }

    for (int i = 0; i < n; ++i) {
      dispatch_event(events_[i]);
    }

    return n;
  }

  /// @brief Blocks until request_stop() is called.
  /// Calls start() internally, then loops poll_once().
  /// Optionally pins to config_.pin_core and enables the hot-path
  /// allocation guard (Debug only).
  void run() noexcept {
    if (!started_) {
      if (!start()) {
        return;
      }
    }

    const int timeout = config_.busy_spin ? 0 : -1;

    while (!stop_.load(std::memory_order_relaxed)) {
      poll_once(timeout);
    }

    // Disable hot-path guard before cleanup.
    if (config_.alloc_guard) {
      mk::sys::thread::set_hot_path_mode(false);
    }
  }

  /// @brief Signals run() to exit after the current epoll batch.
  /// Thread-safe. Safe to call from signal handlers.
  void request_stop() noexcept { stop_.store(true, std::memory_order_relaxed); }

  // ========================================================================
  // Send API — callable from handler callbacks
  // ========================================================================

  /// @brief Queues payload (with framing) into the connection's tx buffer.
  /// Must be called from the event loop thread (single-threaded).
  /// @param id The connection to send to.
  /// @param payload Raw payload bytes (framed by the Framer before sending).
  /// @return false if tx buffer has insufficient space.
  [[nodiscard]] bool send(ConnId id,
                          std::span<const std::byte> payload) noexcept {
    assert(id >= 0 && id < kMaxConns);
    ConnSlot &s = slots_[id];
    assert(s.fd == id);

    auto out = std::as_writable_bytes(
        std::span{s.tx_buf + s.tx_size, kTxBufSize - s.tx_size});

    std::size_t written = framer_.encode(out, payload);
    if (written == 0) {
      return false;
    }

    s.tx_size += static_cast<std::uint32_t>(written);
    return true;
  }

  /// @brief Server-initiated close. on_disconnect() will fire synchronously.
  /// @param id The connection to close.
  void close(ConnId id) noexcept {
    assert(id >= 0 && id < kMaxConns);
    if (slots_[id].fd == id) {
      close_connection(id);
    }
  }

  // ========================================================================
  // Observers
  // ========================================================================

  /// @brief Returns the actual bound port (useful when config.port == 0).
  /// Only valid after listen() returns true.
  [[nodiscard]] std::uint16_t port() const noexcept { return bound_port_; }
};

} // namespace mk::net
