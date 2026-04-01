/**
 * @file exchange_gateway_main.cpp
 * @brief Order gateway process — single client, recv/send thread separation.
 *
 * Each gateway process serves exactly one client (industry standard).
 * Multiple gateway instances run in parallel, each with a unique
 * --gateway_id selecting its dedicated queue pair in shared memory.
 *
 * Thread architecture:
 *   [Recv Thread]  TCP recv → parse → request_queues[gateway_id] push
 *   [Send Thread]  response_queues[gateway_id] pop → serialize → TCP send
 *
 * Single-writer guarantee: all TCP writes go through the send thread.
 * The recv thread queues heartbeat acks and throttle rejects via send_queue.
 *
 * Usage:
 *   ./exchange_gateway --gateway_id=0 --tcp_port=8888 \
 *       --shm_name=/mk_exchange_events
 */

#include "exchange_event.hpp"
#include "message_codec_framer.hpp"
#include "shared/protocol.hpp"
#include "shared/protocol_codec.hpp"
#include "shared_exchange_queue.hpp"

#include "net/message_codec.hpp"
#include "sys/log/signal_logger.hpp"
#include "sys/memory/fixed_spsc_queue.hpp"
#include "sys/memory/mmap_region.hpp"
#include "sys/nano_clock.hpp"
#include "sys/thread/affinity.hpp"

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// -- Command-line flags --

ABSL_FLAG(uint16_t, tcp_port, 8888, "TCP port for this gateway");
ABSL_FLAG(std::string, shm_name, std::string(mk::app::kDefaultShmName),
          "POSIX shared memory name (must match exchange_engine)");
ABSL_FLAG(uint32_t, gateway_id, 0,
          "Gateway slot index (0-based, must be < kMaxGateways)");
ABSL_FLAG(int32_t, pin_core_recv, -1, "CPU core for recv thread");
ABSL_FLAG(int32_t, pin_core_send, -1, "CPU core for send thread");

// -- Global stop flag (signal-safe) --

namespace {

std::atomic_flag g_stop = ATOMIC_FLAG_INIT;

void signal_handler(int /*signum*/) {
  g_stop.test_and_set(std::memory_order_relaxed);
}

[[nodiscard]] bool
should_stop(const mk::app::SharedExchangeRegion &shared) noexcept {
  return g_stop.test(std::memory_order_relaxed) ||
         shared.shutdown.load(std::memory_order_relaxed) != 0;
}

// -- Send helper --

constexpr std::int64_t kSendTimeoutNs = 1'000'000; // 1ms

[[nodiscard]] bool send_full(int fd, const std::byte *data,
                             std::size_t len) noexcept {
  // NOLINTNEXTLINE(*-pro-type-reinterpret-cast)
  const auto *buf = reinterpret_cast<const char *>(data);
  std::size_t total = 0;
  auto deadline = mk::sys::monotonic_nanos() + kSendTimeoutNs;

  while (total < len) {
    const auto sent = ::send(fd, buf + total, len - total, MSG_NOSIGNAL);
    if (sent > 0) {
      total += static_cast<std::size_t>(sent);
    } else if (sent < 0 && errno == EINTR) {
      continue;
    } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (mk::sys::monotonic_nanos() >= deadline) {
        return false;
      }
      continue;
    } else {
      return false;
    }
  }
  return true;
}

// -- Constants --

constexpr std::size_t kRxBufSize = 8192;
constexpr std::size_t kTxBufSize = 512;

/// Message queued from recv thread → send thread for TCP write.
struct PendingSend {
  std::size_t len{0};
  std::byte data[64]{}; // NOLINT(*-avoid-c-arrays) — max TLV frame is
                        // FillReport 40B + header 16B = 56B.
};

// -- Serialize ExchangeEvent → TLV bytes --

std::size_t serialize_one_event(const mk::app::ExchangeEvent &event,
                                std::span<std::byte> buf) noexcept {
  using mk::app::EventType;
  using mk::app::MsgType;

  std::array<std::byte, 256> payload_buf{};
  std::size_t payload_len = 0;

  switch (event.type) {
  case EventType::kOrderAccepted: {
    const mk::app::OrderAck ack{
        .client_order_id = event.client_order_id,
        .exchange_order_id = event.exchange_order_id,
        .send_ts = event.send_ts,
    };
    payload_len = mk::app::serialize_order_ack(payload_buf, ack);
    return mk::app::pack_tcp_message(
        buf, MsgType::kOrderAck,
        std::span<const std::byte>(payload_buf.data(), payload_len));
  }
  case EventType::kOrderRejected: {
    const mk::app::OrderReject reject{
        .client_order_id = event.client_order_id,
        .reason = event.reason,
        .send_ts = event.send_ts,
    };
    payload_len = mk::app::serialize_order_reject(payload_buf, reject);
    return mk::app::pack_tcp_message(
        buf, MsgType::kOrderReject,
        std::span<const std::byte>(payload_buf.data(), payload_len));
  }
  case EventType::kFill: {
    const mk::app::FillReport fill{
        .client_order_id = event.client_order_id,
        .exchange_order_id = event.exchange_order_id,
        .fill_price = event.price,
        .fill_qty = event.qty,
        .remaining_qty = event.remaining_qty,
        .send_ts = event.send_ts,
    };
    payload_len = mk::app::serialize_fill_report(payload_buf, fill);
    return mk::app::pack_tcp_message(
        buf, MsgType::kFillReport,
        std::span<const std::byte>(payload_buf.data(), payload_len));
  }
  case EventType::kCancelAck: {
    const mk::app::CancelAck ack{
        .client_order_id = event.client_order_id,
        .send_ts = event.send_ts,
    };
    payload_len = mk::app::serialize_cancel_ack(payload_buf, ack);
    return mk::app::pack_tcp_message(
        buf, MsgType::kCancelAck,
        std::span<const std::byte>(payload_buf.data(), payload_len));
  }
  case EventType::kCancelRejected: {
    const mk::app::CancelReject reject{
        .client_order_id = event.client_order_id,
        .reason = event.reason,
        .send_ts = event.send_ts,
    };
    payload_len = mk::app::serialize_cancel_reject(payload_buf, reject);
    return mk::app::pack_tcp_message(
        buf, MsgType::kCancelReject,
        std::span<const std::byte>(payload_buf.data(), payload_len));
  }
  case EventType::kModifyAck: {
    const mk::app::ModifyAck ack{
        .client_order_id = event.client_order_id,
        .new_exchange_order_id = event.exchange_order_id,
        .send_ts = event.send_ts,
    };
    payload_len = mk::app::serialize_modify_ack(payload_buf, ack);
    return mk::app::pack_tcp_message(
        buf, MsgType::kModifyAck,
        std::span<const std::byte>(payload_buf.data(), payload_len));
  }
  case EventType::kModifyRejected: {
    const mk::app::ModifyReject reject{
        .client_order_id = event.client_order_id,
        .reason = event.reason,
        .send_ts = event.send_ts,
    };
    payload_len = mk::app::serialize_modify_reject(payload_buf, reject);
    return mk::app::pack_tcp_message(
        buf, MsgType::kModifyReject,
        std::span<const std::byte>(payload_buf.data(), payload_len));
  }
  case EventType::kBBOUpdate:
  case EventType::kSessionAssigned:
    return 0;
  }
  return 0;
}

// -- Parse TLV frame → OrderRequest --

[[nodiscard]] bool parse_frame_to_request(std::span<const std::byte> frame,
                                          std::uint16_t session_id,
                                          std::uint32_t request_seq,
                                          std::uint8_t gateway_id,
                                          mk::app::OrderRequest &req) noexcept {

  mk::net::ParsedMessageView msg{};
  if (!mk::net::unpack_message(frame, msg)) {
    return false;
  }

  const auto msg_type = static_cast<mk::app::MsgType>(msg.header.msg_type);
  req.gateway_id = gateway_id;
  req.session_id = session_id;
  req.request_seq = request_seq;

  switch (msg_type) {
  case mk::app::MsgType::kNewOrder: {
    mk::app::NewOrder order{};
    if (!mk::app::deserialize_new_order(msg.payload, order)) {
      return false;
    }
    req.type = mk::app::OrderRequestType::kNewOrder;
    req.client_order_id = order.client_order_id;
    req.symbol_id = order.symbol_id;
    req.side = order.side;
    req.price = order.price;
    req.qty = order.qty;
    req.send_ts = order.send_ts;
    return true;
  }
  case mk::app::MsgType::kCancelOrder: {
    mk::app::CancelOrder cancel{};
    if (!mk::app::deserialize_cancel_order(msg.payload, cancel)) {
      return false;
    }
    req.type = mk::app::OrderRequestType::kCancelOrder;
    req.client_order_id = cancel.client_order_id;
    req.symbol_id = cancel.symbol_id;
    req.send_ts = cancel.send_ts;
    return true;
  }
  case mk::app::MsgType::kModifyOrder: {
    mk::app::ModifyOrder modify{};
    if (!mk::app::deserialize_modify_order(msg.payload, modify)) {
      return false;
    }
    req.type = mk::app::OrderRequestType::kModifyOrder;
    req.client_order_id = modify.client_order_id;
    req.symbol_id = modify.symbol_id;
    req.new_price = modify.new_price;
    req.new_qty = modify.new_qty;
    req.send_ts = modify.send_ts;
    return true;
  }
  case mk::app::MsgType::kHeartbeat:
    return false; // handled directly by recv thread
  default:
    return false;
  }
}

// ======================================================================
// Recv Thread — accept one client, recv + parse + push to request_queue
// ======================================================================

void recv_thread_main(
    int listen_fd, mk::app::SharedExchangeRegion &shared,
    mk::app::SharedRequestQueue &request_queue,
    mk::sys::memory::FixedSPSCQueue<PendingSend, 128> &send_queue,
    std::atomic<std::uint16_t> &session_id, std::atomic<int> &client_fd_shared,
    std::uint8_t gateway_id, std::int32_t pin_core) noexcept {

  if (pin_core >= 0) {
    auto err = mk::sys::thread::pin_current_thread(
        static_cast<std::uint32_t>(pin_core));
    if (err == 0) {
      mk::sys::log::signal_log("[GW-", gateway_id, "-RECV] Pinned to core ",
                               pin_core, '\n');
    }
  }

  mk::sys::log::signal_log("[GW-", gateway_id, "-RECV] Waiting for client\n");

  // -- Accept exactly one client --
  int client_fd = -1;
  while (!should_stop(shared)) {
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    // NOLINTNEXTLINE(*-pro-type-reinterpret-cast)
    client_fd = ::accept4(listen_fd, reinterpret_cast<sockaddr *>(&peer),
                          &peer_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client_fd >= 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (client_fd < 0) {
    mk::sys::log::signal_log("[GW-", gateway_id,
                             "-RECV] No client, shutting down\n");
    return;
  }

  int one = 1;
  (void)::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  // Share fd with send thread immediately (no Engine round-trip needed).
  client_fd_shared.store(client_fd, std::memory_order_release);

  mk::sys::log::signal_log("[GW-", gateway_id,
                           "-RECV] Client accepted: fd=", client_fd, '\n');

  // Tell engine to assign a session for this client.
  std::uint32_t next_seq = 1;
  const auto connect_seq = next_seq++;
  if (!request_queue.try_push(mk::app::OrderRequest{
          .type = mk::app::OrderRequestType::kClientConnect,
          .gateway_id = gateway_id,
          .request_seq = connect_seq,
          .client_order_id = static_cast<std::uint64_t>(client_fd)}))
      [[unlikely]] {
    mk::sys::log::signal_log("[GW-", gateway_id,
                             "-RECV] request_queue full on connect, closing\n");
    ::close(client_fd);
    g_stop.test_and_set(std::memory_order_relaxed);
    return;
  }

  // -- Recv loop (single client, no epoll needed) --
  char rx_buf[kRxBufSize]{}; // NOLINT(*-avoid-c-arrays)
  std::uint32_t rx_fill = 0;
  const mk::app::MessageCodecFramer framer;

  while (!should_stop(shared)) {
    // Non-blocking recv.
    const auto space = kRxBufSize - rx_fill;
    if (space == 0) {
      break; // rx_buf overflow — protocol error
    }

    const auto nbytes =
        ::recv(client_fd, rx_buf + rx_fill, space, MSG_DONTWAIT);
    if (nbytes > 0) {
      rx_fill += static_cast<std::uint32_t>(nbytes);
    } else if (nbytes == 0) {
      break; // peer closed
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // No data available. Brief sleep so send thread can run
      // (both threads may share the same core when unpinned).
      // In production with dedicated cores, this would be a busy-spin.
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    } else if (errno != EINTR) {
      break; // error
    }

    // Parse TLV frames.
    const auto cur_session = session_id.load(std::memory_order_acquire);
    std::uint32_t consumed = 0;

    while (consumed < rx_fill && !should_stop(shared)) {
      auto remaining = std::as_bytes(
          std::span<const char>{rx_buf + consumed, rx_fill - consumed});

      const auto result = framer.decode(remaining);
      if (result.status == mk::net::FrameStatus::kIncomplete) {
        break;
      }
      if (result.status == mk::net::FrameStatus::kError) {
        goto disconnect; // NOLINT(cppcoreguidelines-avoid-goto)
      }

      // Peek at message type.
      mk::net::ParsedMessageView peek{};
      if (!mk::net::unpack_message(result.payload, peek)) {
        consumed += static_cast<std::uint32_t>(result.frame_size);
        continue;
      }

      const auto msg_type = static_cast<mk::app::MsgType>(peek.header.msg_type);

      if (msg_type == mk::app::MsgType::kHeartbeat) {
        // Queue HeartbeatAck for send thread.
        PendingSend hb{};
        hb.len = mk::app::pack_tcp_message(std::span<std::byte>(hb.data),
                                           mk::app::MsgType::kHeartbeatAck, {});
        if (!send_queue.try_push(hb)) [[unlikely]] {
          goto disconnect; // NOLINT(cppcoreguidelines-avoid-goto)
        }
      } else if (cur_session == 0) {
        // Session not yet assigned — discard.
        mk::sys::log::signal_log("[GW-", gateway_id,
                                 "-RECV] Order discarded (no session)\n");
      } else {
        // Parse and push to engine.
        mk::app::OrderRequest req{};
        const auto seq = next_seq++;
        if (!parse_frame_to_request(result.payload, cur_session, seq,
                                    gateway_id, req)) {
          mk::sys::log::signal_log(
              "[GW-", gateway_id,
              "-RECV] Unknown or malformed message, dropped\n");
        } else {
          if (!request_queue.try_push(req)) [[unlikely]] {
            // Typed reject via send_queue.
            mk::app::EventType rej_type{};
            switch (req.type) {
            case mk::app::OrderRequestType::kCancelOrder:
              rej_type = mk::app::EventType::kCancelRejected;
              break;
            case mk::app::OrderRequestType::kModifyOrder:
              rej_type = mk::app::EventType::kModifyRejected;
              break;
            default:
              rej_type = mk::app::EventType::kOrderRejected;
              break;
            }
            PendingSend rej{};
            const mk::app::ExchangeEvent rej_event{
                .type = rej_type,
                .session_id = cur_session,
                .client_order_id = req.client_order_id,
                .send_ts = req.send_ts,
                .reason = mk::app::RejectReason::kThrottled,
            };
            rej.len =
                serialize_one_event(rej_event, std::span<std::byte>(rej.data));
            if (rej.len > 0 && !send_queue.try_push(rej)) [[unlikely]] {
              goto disconnect; // NOLINT(cppcoreguidelines-avoid-goto)
            }
          }
        }
      }
      consumed += static_cast<std::uint32_t>(result.frame_size);
    }

    // Compact rx_buf.
    if (consumed > 0 && consumed < rx_fill) {
      std::memmove(rx_buf, rx_buf + consumed, rx_fill - consumed);
    }
    rx_fill -= consumed;
    continue;

  disconnect:
    break;
  }

  // Notify engine of disconnect.
  const auto final_session = session_id.load(std::memory_order_relaxed);
  if (final_session != 0) {
    const auto seq = next_seq++;
    if (!request_queue.try_push(mk::app::OrderRequest{
            .type = mk::app::OrderRequestType::kClientDisconnect,
            .gateway_id = gateway_id,
            .session_id = final_session,
            .request_seq = seq})) [[unlikely]] {
      // Known limitation: if this push fails, engine never learns
      // about the disconnect. Orphaned resting orders remain until
      // engine restart (which clears all state via placement new).
      mk::sys::log::signal_log("[GW-", gateway_id,
                               "-RECV] request_queue full on disconnect\n");
    }
  }

  ::close(client_fd);
  // Signal send thread to exit (recv thread is the lifecycle owner).
  g_stop.test_and_set(std::memory_order_relaxed);
  mk::sys::log::signal_log("[GW-", gateway_id, "-RECV] Client disconnected\n");
}

// ======================================================================
// Send Thread — pop engine responses + local sends → TCP
// ======================================================================

void send_thread_main(
    mk::app::SharedExchangeRegion &shared,
    mk::app::SharedEventQueue &response_queue,
    mk::sys::memory::FixedSPSCQueue<PendingSend, 128> &send_queue,
    std::atomic<std::uint16_t> &session_id, std::atomic<int> &client_fd_shared,
    std::uint8_t gateway_id, std::int32_t pin_core) noexcept {

  if (pin_core >= 0) {
    auto err = mk::sys::thread::pin_current_thread(
        static_cast<std::uint32_t>(pin_core));
    if (err == 0) {
      mk::sys::log::signal_log("[GW-", gateway_id, "-SEND] Pinned to core ",
                               pin_core, '\n');
    }
  }

  std::array<std::byte, kTxBufSize> tx_buf{};

  mk::sys::log::signal_log("[GW-", gateway_id, "-SEND] Entering send loop\n");

  while (!should_stop(shared)) {
    // Read client fd from recv thread (set immediately after accept).
    const int fd = client_fd_shared.load(std::memory_order_acquire);

    // Pop engine responses.
    mk::app::ExchangeEvent event{};
    while (response_queue.try_pop(event)) {
      if (event.type == mk::app::EventType::kSessionAssigned) {
        session_id.store(event.session_id, std::memory_order_release);
        mk::sys::log::signal_log(
            "[GW-", gateway_id,
            "-SEND] Session assigned: session=", event.session_id, '\n');
        continue;
      }

      const auto len = serialize_one_event(event, tx_buf);
      if (len == 0 || fd < 0) {
        continue;
      }

      if (!send_full(fd, tx_buf.data(), len)) {
        mk::sys::log::signal_log("[GW-", gateway_id,
                                 "-SEND] Send failed: errno=", errno, '\n');
        ::shutdown(fd, SHUT_RDWR);
        client_fd_shared.store(-1, std::memory_order_relaxed);
        // Stop recv thread too — response path is dead, further requests
        // would be blackholed (engine accepts but client never gets ack).
        g_stop.test_and_set(std::memory_order_relaxed);
        break;
      }
    }

    // Drain locally-generated sends (heartbeat ack, throttle reject).
    PendingSend pending{};
    while (send_queue.try_pop(pending)) {
      const int cur_fd = client_fd_shared.load(std::memory_order_relaxed);
      if (cur_fd >= 0) {
        if (!send_full(cur_fd, pending.data, pending.len)) {
          mk::sys::log::signal_log("[GW-", gateway_id,
                                   "-SEND] Local send failed: errno=", errno,
                                   '\n');
          ::shutdown(cur_fd, SHUT_RDWR);
          client_fd_shared.store(-1, std::memory_order_relaxed);
          g_stop.test_and_set(std::memory_order_relaxed);
          break;
        }
      }
    }

    // Brief sleep when no work — prevents CPU starvation of recv thread
    // when both threads share a core (unpinned mode).
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  mk::sys::log::signal_log("[GW-", gateway_id, "-SEND] Send loop exited\n");
}

} // namespace

// ======================================================================
// main
// ======================================================================

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  const auto tcp_port = absl::GetFlag(FLAGS_tcp_port);
  const auto shm_name = absl::GetFlag(FLAGS_shm_name);
  const auto raw_gateway_id = absl::GetFlag(FLAGS_gateway_id);
  const auto pin_core_recv = absl::GetFlag(FLAGS_pin_core_recv);
  const auto pin_core_send = absl::GetFlag(FLAGS_pin_core_send);

  // Validate BEFORE narrowing cast — uint32_t 256 would silently wrap to 0.
  if (raw_gateway_id >= mk::app::kMaxGateways) {
    mk::sys::log::signal_log("[GATEWAY] gateway_id=", raw_gateway_id,
                             " exceeds kMaxGateways=", mk::app::kMaxGateways,
                             '\n');
    return 1;
  }
  const auto gateway_id = static_cast<std::uint8_t>(raw_gateway_id);

  // -- Signal handling --
  struct sigaction sa {};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  // -- Open shared memory --
  mk::sys::log::signal_log("[GW-", gateway_id,
                           "] Waiting for shared memory: ", shm_name.c_str(),
                           '\n');

  std::optional<mk::sys::memory::MmapRegion> shm_region;
  while (!g_stop.test(std::memory_order_relaxed)) {
    shm_region = mk::sys::memory::MmapRegion::open_shared(
        shm_name, sizeof(mk::app::SharedExchangeRegion),
        mk::sys::memory::ShmMode::kOpenExisting,
        mk::sys::memory::PrefaultPolicy::kPopulateRead);
    if (shm_region) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!shm_region) {
    return 1;
  }

  // NOLINTNEXTLINE(*-pro-type-reinterpret-cast)
  auto *shared =
      reinterpret_cast<mk::app::SharedExchangeRegion *>(shm_region->data());

  while (!g_stop.test(std::memory_order_relaxed) &&
         shared->engine_ready.load(std::memory_order_acquire) == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (g_stop.test(std::memory_order_relaxed)) {
    return 1;
  }

  mk::sys::log::signal_log("[GW-", gateway_id, "] Engine ready.\n");

  // Validate gateway_id against engine's configured num_gateways.
  const auto configured_gateways =
      shared->num_gateways.load(std::memory_order_relaxed);
  if (gateway_id >= configured_gateways) {
    mk::sys::log::signal_log("[GW-", gateway_id,
                             "] gateway_id >= num_gateways (",
                             configured_gateways, "), aborting\n");
    return 1;
  }

  // Claim this gateway slot (prevents duplicate gateway_id).
  // compare_exchange: expected=0 (unclaimed) → set to 1 (claimed).
  std::uint32_t expected = 0;
  if (!shared->gateway_claimed[gateway_id].compare_exchange_strong(
          expected, 1U, std::memory_order_acq_rel)) {
    mk::sys::log::signal_log("[GW-", gateway_id,
                             "] Slot already claimed by another process, "
                             "aborting\n");
    return 1;
  }

  // -- Create TCP listener --
  const int listen_fd =
      ::socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_fd < 0) {
    mk::sys::log::signal_log("[GW-", gateway_id, "] socket() failed\n");
    shared->gateway_claimed[gateway_id].store(0U, std::memory_order_release);
    return 1;
  }

  int one = 1;
  (void)::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(tcp_port);

  // NOLINTNEXTLINE(*-pro-type-reinterpret-cast)
  if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) <
      0) {
    mk::sys::log::signal_log("[GW-", gateway_id, "] bind() failed\n");
    ::close(listen_fd);
    shared->gateway_claimed[gateway_id].store(0U, std::memory_order_release);
    return 1;
  }

  if (::listen(listen_fd, 1) < 0) {
    mk::sys::log::signal_log("[GW-", gateway_id, "] listen() failed\n");
    ::close(listen_fd);
    shared->gateway_claimed[gateway_id].store(0U, std::memory_order_release);
    return 1;
  }

  sockaddr_in bound{};
  socklen_t bound_len = sizeof(bound);
  // NOLINTNEXTLINE(*-pro-type-reinterpret-cast)
  (void)::getsockname(listen_fd, reinterpret_cast<sockaddr *>(&bound),
                      &bound_len);

  mk::sys::log::signal_log("[GW-", gateway_id, "] Listening on port ",
                           ntohs(bound.sin_port), '\n');

  // -- Shared state between threads --
  mk::sys::memory::FixedSPSCQueue<PendingSend, 128> send_queue;
  std::atomic<std::uint16_t> session{0};
  std::atomic<int> client_fd_shared{-1}; // recv sets after accept, send reads

  // -- Launch threads --
  std::thread recv_thr(recv_thread_main, listen_fd, std::ref(*shared),
                       std::ref(shared->request_queues[gateway_id]),
                       std::ref(send_queue), std::ref(session),
                       std::ref(client_fd_shared), gateway_id, pin_core_recv);

  std::thread send_thr(send_thread_main, std::ref(*shared),
                       std::ref(shared->response_queues[gateway_id]),
                       std::ref(send_queue), std::ref(session),
                       std::ref(client_fd_shared), gateway_id, pin_core_send);

  mk::sys::log::signal_log("[GW-", gateway_id,
                           "] Ready. TCP port=", ntohs(bound.sin_port),
                           " shm=", shm_name.c_str(), '\n');

  recv_thr.join();
  send_thr.join();

  // Close listen socket BEFORE releasing slot — prevents a race where
  // a new gateway claims the slot but finds the port still bound.
  ::close(listen_fd);

  // Release gateway slot so this gateway_id can be restarted.
  // NOTE: crash/SIGKILL leaves the slot claimed. Production would use
  // PID + heartbeat for stale claim recovery. For this simulator,
  // an engine restart clears all slots (placement new on SharedExchangeRegion).
  shared->gateway_claimed[gateway_id].store(0U, std::memory_order_release);
  mk::sys::log::signal_log("[GW-", gateway_id, "] Shutdown complete.\n");
  return 0;
}
