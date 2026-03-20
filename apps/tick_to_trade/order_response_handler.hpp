/**
 * @file order_response_handler.hpp
 * @brief TCP order response dispatcher — deserialize + OrderManager dispatch.
 *
 * Counterpart to FeedHandler (UDP market data). Receives parsed TCP messages
 * from the exchange (OrderAck, FillReport, CancelAck, etc.), deserializes
 * them, and dispatches to OrderManager. Tracks consecutive rejects for
 * automatic kill switch triggering.
 *
 * Design:
 *   - Zero allocation (all state is inline).
 *   - Single function call per message (on_tcp_message).
 *   - Returns bool: true = kill switch should be triggered.
 */

#pragma once

#include "tcp_connection.hpp"
#include "latency_tracker.hpp"
#include "order_manager.hpp"

#include "shared/protocol.hpp"
#include "shared/protocol_codec.hpp"

#include "net/message_codec.hpp"

#include "sys/log/async_logger.hpp"
#include "sys/log/log_macros.hpp"
#include "sys/log/signal_logger.hpp"
#include "sys/nano_clock.hpp"

#include <atomic>
#include <cstdint>

namespace mk::app {

class OrderResponseHandler {
public:
  /// Process a parsed TCP message from the exchange.
  /// @param msg  Parsed TLV message (header + payload span).
  /// @param order_mgr OrderManager to dispatch to.
  /// @param tracker  Latency tracker for RTT recording.
  /// @param conn  Connection state (heartbeat recv tracking).
  /// @param log_queue Async logger queue.
  /// @return true if kill switch should be triggered (consecutive rejects
  ///         exceeded threshold).
  [[nodiscard]] bool
  on_tcp_message(const net::ParsedMessageView &msg, OrderManager &order_mgr,
                 LatencyTracker &tracker, ConnectionState &conn,
                 sys::log::LogQueue &log_queue) noexcept {
    auto msg_type = static_cast<MsgType>(msg.header.msg_type);

    switch (msg_type) {
    case MsgType::kOrderAck: {
      OrderAck ack;
      if (deserialize_order_ack(msg.payload, ack)) {
        tracker.record_order_rtt(ack.send_ts);
        order_mgr.on_order_ack(ack);
        consecutive_rejects_ = 0;
        (void)sys::log::log_order(
            log_queue, sys::log::kThreadIdStrategy, sys::log::LogLevel::kInfo,
            sys::log::OrderEvent::kOrderAck, 0, 0, 0, 0, ack.client_order_id,
            ack.exchange_order_id);
      } else {
        parse_errors_.fetch_add(1, std::memory_order_relaxed);
      }
      break;
    }
    case MsgType::kOrderReject: {
      OrderReject rej;
      if (deserialize_order_reject(msg.payload, rej)) {
        order_mgr.on_order_reject(rej);
        ++consecutive_rejects_;
        (void)sys::log::log_order(
            log_queue, sys::log::kThreadIdStrategy, sys::log::LogLevel::kWarn,
            sys::log::OrderEvent::kOrderReject, 0, 0, 0, 0,
            rej.client_order_id);
        if (consecutive_rejects_ >= kMaxConsecutiveRejects) {
          return true; // Signal caller to trigger kill switch.
        }
      } else {
        parse_errors_.fetch_add(1, std::memory_order_relaxed);
      }
      break;
    }
    case MsgType::kFillReport: {
      FillReport fill;
      if (deserialize_fill_report(msg.payload, fill)) {
        tracker.record_order_rtt(fill.send_ts);
        order_mgr.on_fill(fill);
        consecutive_rejects_ = 0;
        (void)sys::log::log_order(
            log_queue, sys::log::kThreadIdStrategy, sys::log::LogLevel::kInfo,
            sys::log::OrderEvent::kFill, 0, 0, fill.fill_price, fill.fill_qty,
            fill.client_order_id, fill.exchange_order_id, fill.remaining_qty);
      } else {
        parse_errors_.fetch_add(1, std::memory_order_relaxed);
      }
      break;
    }
    case MsgType::kCancelAck: {
      CancelAck ack;
      if (deserialize_cancel_ack(msg.payload, ack)) {
        order_mgr.on_cancel_ack(ack);
        (void)sys::log::log_order(
            log_queue, sys::log::kThreadIdStrategy, sys::log::LogLevel::kInfo,
            sys::log::OrderEvent::kCancelAck, 0, 0, 0, 0, ack.client_order_id);
      } else {
        parse_errors_.fetch_add(1, std::memory_order_relaxed);
      }
      break;
    }
    case MsgType::kCancelReject: {
      CancelReject cr;
      if (deserialize_cancel_reject(msg.payload, cr)) {
        order_mgr.on_cancel_reject(cr);
        (void)sys::log::log_order(
            log_queue, sys::log::kThreadIdStrategy, sys::log::LogLevel::kWarn,
            sys::log::OrderEvent::kCancelReject, 0, 0, 0, 0,
            cr.client_order_id);
      } else {
        parse_errors_.fetch_add(1, std::memory_order_relaxed);
      }
      break;
    }
    case MsgType::kModifyAck: {
      ModifyAck ack;
      if (deserialize_modify_ack(msg.payload, ack)) {
        tracker.record_order_rtt(ack.send_ts);
        order_mgr.on_modify_ack(ack);
        (void)sys::log::log_order(
            log_queue, sys::log::kThreadIdStrategy, sys::log::LogLevel::kInfo,
            sys::log::OrderEvent::kModifyAck, 0, 0, 0, 0, ack.client_order_id,
            ack.new_exchange_order_id);
      } else {
        parse_errors_.fetch_add(1, std::memory_order_relaxed);
      }
      break;
    }
    case MsgType::kModifyReject: {
      ModifyReject rej;
      if (deserialize_modify_reject(msg.payload, rej)) {
        order_mgr.on_modify_reject(rej);
        (void)sys::log::log_order(
            log_queue, sys::log::kThreadIdStrategy, sys::log::LogLevel::kWarn,
            sys::log::OrderEvent::kModifyReject, 0, 0, 0, 0,
            rej.client_order_id);
      } else {
        parse_errors_.fetch_add(1, std::memory_order_relaxed);
      }
      break;
    }
    case MsgType::kHeartbeatAck:
      conn.last_hb_recv = sys::monotonic_nanos();
      conn.heartbeats_recv.fetch_add(1, std::memory_order_relaxed);
      (void)sys::log::log_connection(
          log_queue, sys::log::kThreadIdStrategy, sys::log::LogLevel::kDebug,
          sys::log::ConnectionEvent::kHeartbeatRecv);
      break;
    default:
      unknown_types_.fetch_add(1, std::memory_order_relaxed);
      sys::log::signal_log("[PIPELINE] Unknown msg_type=",
                           msg.header.msg_type, '\n');
      break;
    }

    return false;
  }

  // -- Observers --

  [[nodiscard]] std::uint32_t consecutive_rejects() const noexcept {
    return consecutive_rejects_;
  }
  [[nodiscard]] std::uint64_t parse_errors() const noexcept {
    return parse_errors_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t unknown_types() const noexcept {
    return unknown_types_.load(std::memory_order_relaxed);
  }

private:
  static constexpr std::uint32_t kMaxConsecutiveRejects = 5;

  std::uint32_t consecutive_rejects_{0};

  // Diagnostic counters — std::atomic for cross-thread monitoring safety.
  std::atomic<std::uint64_t> parse_errors_{0};
  std::atomic<std::uint64_t> unknown_types_{0};
};

} // namespace mk::app
