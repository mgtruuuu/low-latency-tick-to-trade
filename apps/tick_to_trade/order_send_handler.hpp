/**
 * @file order_send_handler.hpp
 * @brief TCP order send dispatcher — risk check + serialize + send.
 *
 * Counterpart to OrderResponseHandler (inbound responses). Handles outbound
 * order path: receives a strategy Signal, checks for modify-vs-new, risk
 * checks via OrderManager, serializes the wire message, and sends over TCP.
 *
 * Design:
 *   - Zero allocation (all buffers are pre-allocated spans).
 *   - Marked [[gnu::noinline]] — the order path executes on <1% of ticks,
 *     so its instructions should not evict the hot loop's L1i cache lines.
 *   - Returns bool: true = send succeeded (or no send), false = connection
 *     dead (caller must disconnect and reconnect).
 */

#pragma once

#include "tcp_connection.hpp"
#include "latency_tracker.hpp"
#include "order_manager.hpp"

#include "shared/protocol.hpp"
#include "shared/protocol_codec.hpp"

#include "net/tcp_socket.hpp"

#include "sys/log/async_logger.hpp"
#include "sys/log/log_macros.hpp"
#include "sys/nano_clock.hpp"

#include <atomic>
#include <cstdint>
#include <span>

namespace mk::app {

class OrderSendHandler {
public:
  // -- Observers --

  [[nodiscard]] std::uint64_t orders_serialized() const noexcept {
    return orders_serialized_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t modifies_serialized() const noexcept {
    return modifies_serialized_.load(std::memory_order_relaxed);
  }
  /// Order path: risk check → serialize → TCP send.
  /// Marked noinline to keep the hot loop's L1i footprint small — order
  /// path executes on <1% of ticks, so its instructions should not evict
  /// the hot loop's icache lines.
  /// @return true if all sends succeeded (or no send attempted),
  ///         false if connection is dead (caller must disconnect).
  [[gnu::noinline]] [[nodiscard]] bool
  on_signal(const Signal &signal, OrderManager &order_mgr,
            LatencyTracker &tracker, net::TcpSocket &tcp_sock,
            std::span<std::byte> scratch, std::span<std::byte> tcp_tx_buf,
            std::uint64_t t0, ConnectionState &conn,
            sys::log::LogQueue &log_queue) noexcept {
#ifdef PROFILE_STAGES
    auto t3 = sys::rdtsc();
#endif

    // Check if we should modify a resting order instead of placing new.
    ModifyOrder modify;
    if (order_mgr.check_modify(signal, modify)) {
      // Stage 4a: Serialize and send ModifyOrder.
      bool send_ok = true;
      auto plen = serialize_modify_order(scratch, modify);
      if (plen > 0) [[likely]] {
        auto tlen = pack_tcp_message(tcp_tx_buf, MsgType::kModifyOrder,
                                     std::span{scratch.data(), plen});
        if (tlen > 0) [[likely]] {
          auto result = tcp_sock.send_nonblocking(
              reinterpret_cast<const char *>(tcp_tx_buf.data()), tlen);
          send_ok = check_send_result(result, conn, "ModifyOrder");
        }
      }

      modifies_serialized_.fetch_add(1, std::memory_order_relaxed);
      (void)sys::log::log_order(
          log_queue, sys::log::kThreadIdStrategy, sys::log::LogLevel::kInfo,
          sys::log::OrderEvent::kModifySent, modify.symbol_id, 0,
          modify.new_price, modify.new_qty, modify.client_order_id);

      auto t4 = sys::rdtsc();
#ifdef PROFILE_STAGES
      tracker.record_order_send(t4 - t3);
#endif
      tracker.record_tick_to_trade(t0, t4);
      return send_ok;
    }

    // No resting order to modify — place a new order.
    NewOrder order;
    if (!order_mgr.on_signal(signal, order)) {
      return true; // Risk limit breached — no send attempted.
    }

    // Stage 4b: Serialize and send NewOrder.
    auto plen = serialize_new_order(scratch, order);
    if (plen == 0) [[unlikely]] {
      return true;
    }

    auto tlen = pack_tcp_message(tcp_tx_buf, MsgType::kNewOrder,
                                 std::span{scratch.data(), plen});
    if (tlen == 0) [[unlikely]] {
      return true;
    }

    auto send_result = tcp_sock.send_nonblocking(
        reinterpret_cast<const char *>(tcp_tx_buf.data()), tlen);
    const bool send_ok = check_send_result(send_result, conn, "NewOrder");

    orders_serialized_.fetch_add(1, std::memory_order_relaxed);
    (void)sys::log::log_order(
        log_queue, sys::log::kThreadIdStrategy, sys::log::LogLevel::kInfo,
        sys::log::OrderEvent::kNewOrder, order.symbol_id,
        static_cast<std::uint8_t>(order.side), order.price, order.qty,
        order.client_order_id);

    auto t4 = sys::rdtsc();
#ifdef PROFILE_STAGES
    tracker.record_order_send(t4 - t3);
#endif
    tracker.record_tick_to_trade(t0, t4);
    return send_ok;
  }

private:
  // Diagnostic counters — std::atomic for cross-thread monitoring safety.
  std::atomic<std::uint64_t> orders_serialized_{0};
  std::atomic<std::uint64_t> modifies_serialized_{0};
};

} // namespace mk::app
