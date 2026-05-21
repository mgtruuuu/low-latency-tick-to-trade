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

#include "latency_tracker.hpp"
#include "order_manager.hpp"
#include "tcp_connection.hpp"

#include "shared/protocol.hpp"
#include "shared/protocol_codec.hpp"

#include "net/tcp_socket.hpp"

#include "pipeline_log_push.hpp"
#include "sys/nano_clock.hpp"

#include <atomic>
#include <cstdint>
#include <ctime>
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
            std::uint64_t t0, std::int64_t kernel_recv_ns,
            ConnectionState &conn, PipelineLogQueue &log_queue) noexcept {
#ifdef PROFILE_STAGES
    auto t3 = sys::rdtsc();
#endif

    // Check if we should modify a resting order instead of placing new.
    ModifyOrder modify;
    if (order_mgr.check_modify(signal, modify)) {
      // Stage 4a: Serialize and send ModifyOrder.
      bool send_ok = true;
      bool sent = false;
      std::uint64_t t4 = 0;
      std::int64_t post_send_ns = 0;
      auto plen = serialize_modify_order(scratch, modify);
      if (plen > 0) [[likely]] {
        auto tlen = pack_tcp_message(tcp_tx_buf, MsgType::kModifyOrder,
                                     std::span{scratch.data(), plen});
        if (tlen > 0) [[likely]] {
          auto result = tcp_sock.send_nonblocking(
              reinterpret_cast<const char *>(tcp_tx_buf.data()), tlen);
          // Capture timestamps immediately after send_nonblocking returns —
          // before check_send_result, counters, and log_order — so the
          // "post-send-return" semantic of the Tick-to-Trade headline is
          // exact (does not include post-send bookkeeping).
          t4 = sys::rdtsc();
          post_send_ns = post_send_realtime_ns();
          sent = true;
          send_ok = check_send_result(result, conn, "ModifyOrder");
        }
      }

      modifies_serialized_.fetch_add(1, std::memory_order_relaxed);
      (void)log_order(log_queue, kThreadIdStrategy, LogLevel::kInfo,
                      OrderEvent::kModifySent, modify.symbol_id, 0,
                      modify.new_price, modify.new_qty, modify.client_order_id);

      if (sent) {
#ifdef PROFILE_STAGES
        tracker.record_order_send(t4 - t3);
#endif
        tracker.record_tick_to_trade(t0, t4);
        tracker.record_kernel_tick_to_trade(kernel_recv_ns, post_send_ns);
      }
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
    // Capture timestamps immediately after send_nonblocking returns —
    // before check_send_result, counters, and log_order — so the
    // "post-send-return" semantic of the Tick-to-Trade headline is exact
    // (does not include post-send bookkeeping).
    const auto t4 = sys::rdtsc();
    const std::int64_t post_send_ns = post_send_realtime_ns();
    const bool send_ok = check_send_result(send_result, conn, "NewOrder");

    orders_serialized_.fetch_add(1, std::memory_order_relaxed);
    (void)log_order(log_queue, kThreadIdStrategy, LogLevel::kInfo,
                    OrderEvent::kNewOrder, order.symbol_id,
                    static_cast<std::uint8_t>(order.side), order.price,
                    order.qty, order.client_order_id);

#ifdef PROFILE_STAGES
    tracker.record_order_send(t4 - t3);
#endif
    tracker.record_tick_to_trade(t0, t4);
    tracker.record_kernel_tick_to_trade(kernel_recv_ns, post_send_ns);
    return send_ok;
  }

private:
  /// CLOCK_REALTIME ns timestamp for the kernel-RX → post-send-return metric.
  /// vDSO-accelerated; ~20-30 ns on modern Linux. Called only when an order
  /// is actually sent (~order_rate/sec), so per-second cost is negligible.
  [[nodiscard]] static std::int64_t post_send_realtime_ns() noexcept {
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return (static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000LL) +
           ts.tv_nsec;
  }

  // Diagnostic counters — std::atomic for cross-thread monitoring safety.
  std::atomic<std::uint64_t> orders_serialized_{0};
  std::atomic<std::uint64_t> modifies_serialized_{0};
};

} // namespace mk::app
