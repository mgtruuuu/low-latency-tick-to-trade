/**
 * @file order_gateway.hpp
 * @brief TCP order gateway for the simulated exchange.
 *
 * Receives orders from the trading pipeline via TCP, processes them
 * through per-symbol MatchingEngines, and sends back acks/fills/rejects.
 *
 * Uses message_codec.hpp TLV format for all TCP messages. The gateway
 * deserializes incoming messages, routes by symbol_id to the correct
 * MatchingEngine, and serializes response messages into the caller-provided
 * send buffer.
 *
 * Multi-symbol design:
 *   Each symbol has its own MatchingEngine and MarketDataPublisher.
 *   The gateway routes NewOrder and CancelOrder by symbol_id. Exchange
 *   order IDs are globally unique across all symbols. The client-to-
 *   exchange ID mapping (id_map_) is shared across symbols.
 *
 * Session management:
 *   FIX ClOrdID (tag 11) is unique per session, not globally. Two
 *   clients could send client_order_id=1 independently. The gateway
 *   assigns a monotonic session_id on connect and uses a composite key
 *   (session_id << 48 | client_order_id) for the ID mapping. This
 *   makes the key globally unique without requiring cross-session
 *   coordination. 48-bit client_order_id supports 281 trillion orders
 *   per session — sufficient for any trading day.
 *
 * Design:
 *   - Pre-allocated response buffer.
 *   - MatchingEngine per symbol (up to MaxSymbols).
 *   - FixedHashMap for composite key → exchange_order_id mapping.
 */

#pragma once

#include "market_data_publisher.hpp"
#include "shared/protocol.hpp"
#include "shared/protocol_codec.hpp"

#include "algo/matching_engine.hpp"
#include "ds/fixed_hash_map.hpp"
#include "net/message_codec.hpp"
#include "net/udp_socket_concept.hpp"
#include "sys/log/signal_logger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace mk::app {

template <net::UdpSendable UdpSock = net::UdpSocket, std::size_t MaxSymbols = 2>
class OrderGateway {
public:
  // Maximum response size the gateway can emit for one inbound message.
  // NewOrder/Modify can produce 1 ack + up to MatchingEngine::kMaxFillsPerMatch
  // fill reports.
  static constexpr std::size_t kMaxResponseBytes =
      (net::kMessageHeaderSize + kOrderAckWireSize) +
      (algo::MatchingEngine<>::kMaxFillsPerMatch *
       (net::kMessageHeaderSize + kFillReportWireSize));

  /// Register a symbol slot. Call once per symbol at startup.
  /// @param symbol_id 1-based symbol identifier (must be in [1, MaxSymbols]).
  /// @param publisher This symbol's MarketDataPublisher.
  void register_symbol(std::uint32_t symbol_id,
                       MarketDataPublisher<UdpSock> &publisher) noexcept {
    if (symbol_id == 0 || symbol_id > MaxSymbols) {
      std::abort();
    }
    const auto idx = symbol_id - 1;
    symbols_[idx].publisher = &publisher;
    symbols_[idx].active = true;
  }

  /// Assign a session ID for a newly connected client.
  /// Returns the assigned session_id for logging/diagnostics.
  [[nodiscard]] std::uint16_t on_client_connect() noexcept {
    if (next_session_id_ == 0) {
      next_session_id_ = 1;
    }
    return next_session_id_++;
  }

  /// Handle one client disconnect — cancel only that session's resting orders.
  void on_client_disconnect(std::uint16_t session_id) noexcept {
    if (session_id == 0) {
      return;
    }

    // Kill switch scoped to this session only.
    // NOTE: keys_to_erase is 8 KiB with current kMaxIdMappings=1024.
    // This is a cold-path (disconnect-only) stack tradeoff to avoid heap alloc.
    std::array<std::uint64_t, kMaxIdMappings> keys_to_erase{};
    std::size_t erase_count = 0;

    id_map_.for_each([this, session_id, &keys_to_erase,
                      &erase_count](const std::uint64_t &key,
                                    IdEntry &entry) noexcept {
      if (extract_session_id(key) != session_id) {
        return;
      }
      (void)symbols_[entry.symbol_id - 1].engine.cancel_order(
          entry.exchange_id);
      if (erase_count < keys_to_erase.size()) {
        keys_to_erase[erase_count++] = key;
      }
    });

    for (std::size_t i = 0; i < erase_count; ++i) {
      (void)id_map_.erase(keys_to_erase[i]);
    }
  }

  /// Process a complete TLV message from a connected client.
  /// Writes response messages into send_buf.
  /// @return Total bytes written to send_buf (may contain multiple messages).
  [[nodiscard]] std::size_t
  on_message(std::uint16_t session_id, std::span<const std::byte> raw_tlv,
             std::span<std::byte> send_buf) noexcept {
    if (session_id == 0) [[unlikely]] {
      return 0;
    }

    net::ParsedMessageView msg;
    if (!net::unpack_message(raw_tlv, msg)) [[unlikely]] {
      sys::log::signal_log("[GATEWAY] Failed to unpack TLV message\n");
      return 0;
    }

    const auto msg_type = static_cast<MsgType>(msg.header.msg_type);
    const auto required_capacity = min_response_capacity(msg_type);
    if (send_buf.size() < required_capacity) [[unlikely]] {
      sys::log::signal_log("[GATEWAY] Response buffer too small: have=",
                           send_buf.size(), " need>=", required_capacity,
                           " msg_type=", msg.header.msg_type, '\n');
      return 0;
    }

    switch (msg_type) {
    case MsgType::kNewOrder:
      return handle_new_order(session_id, msg.payload, send_buf);
    case MsgType::kCancelOrder:
      return handle_cancel_order(session_id, msg.payload, send_buf);
    case MsgType::kModifyOrder:
      return handle_modify_order(session_id, msg.payload, send_buf);
    case MsgType::kHeartbeat:
      // Reply with empty HeartbeatAck (TLV header only, no payload).
      return pack_tcp_message(send_buf, MsgType::kHeartbeatAck, {});
    default:
      sys::log::signal_log("[GATEWAY] Unknown msg_type=", msg.header.msg_type,
                           '\n');
      return 0;
    }
  }

  /// Access the first symbol's engine (for backward-compatible logging).
  [[nodiscard]] const algo::MatchingEngine<> &engine() const noexcept {
    return symbols_[0].engine;
  }

  /// Access a specific symbol's engine.
  [[nodiscard]] const algo::MatchingEngine<> &
  engine(std::uint32_t symbol_id) const noexcept {
    return symbols_[symbol_id - 1].engine;
  }

private:
  struct SymbolSlot {
    algo::MatchingEngine<> engine;
    MarketDataPublisher<UdpSock> *publisher = nullptr;
    bool active = false;
  };

  std::array<SymbolSlot, MaxSymbols> symbols_{};
  std::uint64_t next_exchange_id_{1};

  // Session management — FIX ClOrdID is per-session, not global.
  // Composite key: (session_id << 48) | client_order_id.
  // 16-bit session_id supports 65535 sessions. 48-bit client_order_id
  // supports 281 trillion orders per session.
  std::uint16_t next_session_id_{1};

  /// Combine session_id and client_order_id into a globally unique key.
  [[nodiscard]] static std::uint64_t
  make_composite_key(std::uint16_t session_id,
                     std::uint64_t client_order_id) noexcept {
    return (static_cast<std::uint64_t>(session_id) << 48) |
           (client_order_id & 0x0000'FFFF'FFFF'FFFF);
  }

  [[nodiscard]] static std::uint16_t
  extract_session_id(std::uint64_t composite_key) noexcept {
    return static_cast<std::uint16_t>((composite_key >> 48) & 0xFFFF);
  }

  // Client-to-exchange ID mapping for cancel/modify support.
  // Only contains resting (unfilled) orders. Entries are removed on
  // full fill or successful cancel.
  //
  // Key: composite (session_id << 48 | client_order_id) via
  // make_composite_key(). FIX ClOrdID (tag 11) is unique per session,
  // not globally — two clients could both send client_order_id=1.
  // The composite key makes it globally unique.
  //
  // NOTE: This is a test harness — 1024 is sufficient for simulation.
  // A production exchange would use HashMap + MmapRegion for larger capacity.
  struct IdEntry {
    std::uint64_t exchange_id;
    algo::Side side;
    std::uint32_t symbol_id; // needed for cancel routing on disconnect
  };
  static constexpr std::size_t kMaxIdMappings = 1024;
  ds::FixedHashMap<std::uint64_t, IdEntry, kMaxIdMappings> id_map_;

  // Scratch buffer for serializing response payloads before TLV wrapping.
  std::array<std::byte, 256> payload_buf_{};

  [[nodiscard]] static constexpr std::size_t
  min_response_capacity(MsgType msg_type) noexcept {
    constexpr std::size_t kAckWithNoPayload = net::kMessageHeaderSize;
    constexpr std::size_t kCancelAckBytes =
        net::kMessageHeaderSize + kCancelAckWireSize;
    constexpr std::size_t kCancelRejectBytes =
        net::kMessageHeaderSize + kCancelRejectWireSize;
    constexpr std::size_t kModifyRejectBytes =
        net::kMessageHeaderSize + kModifyRejectWireSize;
    constexpr std::size_t kOrderRejectBytes =
        net::kMessageHeaderSize + kOrderRejectWireSize;
    constexpr std::size_t kCancelResponseBytes =
        (kCancelAckBytes > kCancelRejectBytes) ? kCancelAckBytes
                                               : kCancelRejectBytes;

    switch (msg_type) {
    case MsgType::kNewOrder:
    case MsgType::kModifyOrder:
      return kMaxResponseBytes;
    case MsgType::kCancelOrder:
      return kCancelResponseBytes;
    case MsgType::kHeartbeat:
      return kAckWithNoPayload;
    default:
      return kModifyRejectBytes > kOrderRejectBytes ? kModifyRejectBytes
                                                    : kOrderRejectBytes;
    }
  }

  /// Validate and convert 1-based symbol_id to 0-based index.
  /// @return true if valid, false otherwise.
  [[nodiscard]] bool symbol_index(std::uint32_t symbol_id,
                                  std::size_t &idx) const noexcept {
    if (symbol_id == 0 || symbol_id > MaxSymbols) {
      return false;
    }
    idx = symbol_id - 1;
    return symbols_[idx].active;
  }

  std::size_t handle_new_order(std::uint16_t session_id,
                               std::span<const std::byte> payload,
                               std::span<std::byte> send_buf) noexcept {
    NewOrder order;
    if (!deserialize_new_order(payload, order)) [[unlikely]] {
      sys::log::signal_log("[GATEWAY] Failed to deserialize NewOrder\n");
      return 0;
    }

    // Route to the correct symbol's MatchingEngine.
    std::size_t sym_idx = 0;
    if (!symbol_index(order.symbol_id, sym_idx)) [[unlikely]] {
      return send_reject(order.client_order_id, RejectReason::kUnknown,
                         order.send_ts, send_buf);
    }
    auto &slot = symbols_[sym_idx];

    std::size_t total_written = 0;

    // Submit to matching engine.
    const auto exchange_id = next_exchange_id_++;
    const auto result = slot.engine.submit_order(exchange_id, order.side,
                                                 order.price, order.qty);

    // Was the order accepted? (either rested or fully filled)
    if (result.rested || !result.fills.empty()) {
      // Send OrderAck.
      const OrderAck ack{.client_order_id = order.client_order_id,
                         .exchange_order_id = exchange_id,
                         .send_ts = order.send_ts};

      const auto ack_len = serialize_order_ack(payload_buf_, ack);
      if (ack_len > 0) {
        const auto tlv_len = pack_tcp_message(
            send_buf.subspan(total_written), MsgType::kOrderAck,
            std::span{payload_buf_.data(), ack_len});
        total_written += tlv_len;
      }

      // Send FillReports for each fill.
      algo::Qty remaining = order.qty;

      for (const auto &fill : result.fills) {
        remaining -= fill.qty;

        const FillReport fr{.client_order_id = order.client_order_id,
                            .exchange_order_id = exchange_id,
                            .fill_price = fill.price,
                            .fill_qty = fill.qty,
                            .remaining_qty = remaining,
                            .send_ts = order.send_ts};

        const auto fr_len = serialize_fill_report(payload_buf_, fr);
        if (fr_len > 0) {
          const auto tlv_len = pack_tcp_message(
              send_buf.subspan(total_written), MsgType::kFillReport,
              std::span{payload_buf_.data(), fr_len});
          total_written += tlv_len;
        }

        // Publish trade event via market data feed.
        slot.publisher->publish_trade(order.side, fill.price, fill.qty);

        // Publish real BBO from OrderBook.
        publish_bbo_from_book(slot);
      }

      // If order is resting (not fully filled), register ID mapping
      // so the client can cancel/modify it later by client_order_id.
      if (result.rested) {
        (void)id_map_.insert(make_composite_key(session_id, order.client_order_id),
                              IdEntry{exchange_id, order.side,
                                      order.symbol_id});
      }
    } else {
      // Order rejected (duplicate ID, book full, etc.).
      total_written +=
          send_reject(order.client_order_id, RejectReason::kBookFull,
                      order.send_ts, send_buf.subspan(total_written));
    }

    return total_written;
  }

  std::size_t handle_cancel_order(std::uint16_t session_id,
                                  std::span<const std::byte> payload,
                                  std::span<std::byte> send_buf) noexcept {
    CancelOrder cancel;
    if (!deserialize_cancel_order(payload, cancel)) [[unlikely]] {
      sys::log::signal_log("[GATEWAY] Failed to deserialize CancelOrder\n");
      return 0;
    }

    // Route to the correct symbol's MatchingEngine.
    std::size_t sym_idx = 0;
    if (!symbol_index(cancel.symbol_id, sym_idx)) [[unlikely]] {
      return send_cancel_reject(cancel.client_order_id, RejectReason::kUnknown,
                                cancel.send_ts, send_buf);
    }
    auto &slot = symbols_[sym_idx];

    // Look up client_order_id -> exchange_order_id mapping.
    const auto key = make_composite_key(session_id, cancel.client_order_id);
    const auto *entry = id_map_.find(key);
    if (!entry) {
      return send_cancel_reject(cancel.client_order_id, RejectReason::kUnknown,
                                cancel.send_ts, send_buf);
    }

    // Cancel the order in the matching engine.
    const bool cancelled = slot.engine.cancel_order(entry->exchange_id);
    if (!cancelled) {
      return send_cancel_reject(cancel.client_order_id, RejectReason::kUnknown,
                                cancel.send_ts, send_buf);
    }

    // Cancel succeeded — remove from ID mapping.
    (void)id_map_.erase(key);

    const CancelAck ca{.client_order_id = cancel.client_order_id,
                       .send_ts = cancel.send_ts};

    const auto ca_len = serialize_cancel_ack(payload_buf_, ca);
    if (ca_len > 0) {
      return pack_tcp_message(send_buf, MsgType::kCancelAck,
                              std::span{payload_buf_.data(), ca_len});
    }
    return 0;
  }

  std::size_t handle_modify_order(std::uint16_t session_id,
                                  std::span<const std::byte> payload,
                                  std::span<std::byte> send_buf) noexcept {
    ModifyOrder modify;
    if (!deserialize_modify_order(payload, modify)) [[unlikely]] {
      sys::log::signal_log("[GATEWAY] Failed to deserialize ModifyOrder\n");
      return 0;
    }

    // Route to the correct symbol's MatchingEngine.
    std::size_t sym_idx = 0;
    if (!symbol_index(modify.symbol_id, sym_idx)) [[unlikely]] {
      return send_modify_reject(modify.client_order_id, RejectReason::kUnknown,
                                modify.send_ts, send_buf);
    }
    auto &slot = symbols_[sym_idx];

    // Look up client_order_id -> IdEntry mapping.
    const auto modify_key =
        make_composite_key(session_id, modify.client_order_id);
    const auto *entry = id_map_.find(modify_key);
    if (!entry) {
      return send_modify_reject(modify.client_order_id,
                                RejectReason::kOrderNotFound, modify.send_ts,
                                send_buf);
    }

    // Assign new exchange ID for the modified order.
    const auto new_exchange_id = next_exchange_id_++;

    // Cancel old + re-submit with new price/qty.
    const auto result = slot.engine.modify_order(
        entry->exchange_id, entry->side, modify.new_price, modify.new_qty,
        new_exchange_id);

    if (!result.success) {
      return send_modify_reject(modify.client_order_id,
                                RejectReason::kOrderNotFound, modify.send_ts,
                                send_buf);
    }

    // Save entry fields before erasing — accessing a logically-erased
    // slot (tombstone) is undefined behaviour even though it works in
    // practice (tombstone deletion preserves memory contents).
    const auto saved_side = entry->side;
    const auto saved_symbol_id = entry->symbol_id;

    // Update ID mapping: remove old, insert new (if resting).
    (void)id_map_.erase(modify_key);
    if (result.rested) {
      (void)id_map_.insert(modify_key,
                            IdEntry{new_exchange_id, saved_side,
                                    saved_symbol_id});
    }

    std::size_t total_written = 0;

    // Send ModifyAck.
    const ModifyAck ack{.client_order_id = modify.client_order_id,
                        .new_exchange_order_id = new_exchange_id,
                        .send_ts = modify.send_ts};

    const auto ack_len = serialize_modify_ack(payload_buf_, ack);
    if (ack_len > 0) {
      const auto tlv_len = pack_tcp_message(
          send_buf.subspan(total_written), MsgType::kModifyAck,
          std::span{payload_buf_.data(), ack_len});
      total_written += tlv_len;
    }

    // Send FillReports if the new price crossed opposite side.
    algo::Qty remaining = modify.new_qty;

    for (const auto &fill : result.fills) {
      remaining -= fill.qty;

      const FillReport fr{.client_order_id = modify.client_order_id,
                          .exchange_order_id = new_exchange_id,
                          .fill_price = fill.price,
                          .fill_qty = fill.qty,
                          .remaining_qty = remaining,
                          .send_ts = modify.send_ts};

      const auto fr_len = serialize_fill_report(payload_buf_, fr);
      if (fr_len > 0) {
        const auto tlv_len = pack_tcp_message(
            send_buf.subspan(total_written), MsgType::kFillReport,
            std::span{payload_buf_.data(), fr_len});
        total_written += tlv_len;
      }

      slot.publisher->publish_trade(saved_side, fill.price, fill.qty);
      publish_bbo_from_book(slot);
    }

    return total_written;
  }

  /// Query real OrderBook for best bid/ask and publish via UDP multicast.
  void publish_bbo_from_book(SymbolSlot &slot) noexcept {
    const auto best_bid = slot.engine.book().best_bid();
    const auto best_ask = slot.engine.book().best_ask();

    if (best_bid) {
      slot.publisher->publish_update(algo::Side::kBid, *best_bid,
                                     static_cast<algo::Qty>(100));
    }
    if (best_ask) {
      slot.publisher->publish_update(algo::Side::kAsk, *best_ask,
                                     static_cast<algo::Qty>(100));
    }
  }

  /// Helper: send an OrderReject message.
  std::size_t send_reject(std::uint64_t client_order_id, RejectReason reason,
                          std::int64_t send_ts,
                          std::span<std::byte> send_buf) noexcept {
    const OrderReject rej{.client_order_id = client_order_id,
                          .reason = reason,
                          .send_ts = send_ts};
    const auto rej_len = serialize_order_reject(payload_buf_, rej);
    if (rej_len > 0) {
      return pack_tcp_message(send_buf, MsgType::kOrderReject,
                              std::span{payload_buf_.data(), rej_len});
    }
    return 0;
  }

  /// Helper: send a CancelReject message.
  std::size_t send_cancel_reject(std::uint64_t client_order_id,
                                 RejectReason reason, std::int64_t send_ts,
                                 std::span<std::byte> send_buf) noexcept {
    const CancelReject cr{.client_order_id = client_order_id,
                          .reason = reason,
                          .send_ts = send_ts};
    const auto cr_len = serialize_cancel_reject(payload_buf_, cr);
    if (cr_len > 0) {
      return pack_tcp_message(send_buf, MsgType::kCancelReject,
                              std::span{payload_buf_.data(), cr_len});
    }
    return 0;
  }

  /// Helper: send a ModifyReject message.
  std::size_t send_modify_reject(std::uint64_t client_order_id,
                                 RejectReason reason, std::int64_t send_ts,
                                 std::span<std::byte> send_buf) noexcept {
    const ModifyReject mr{.client_order_id = client_order_id,
                          .reason = reason,
                          .send_ts = send_ts};
    const auto mr_len = serialize_modify_reject(payload_buf_, mr);
    if (mr_len > 0) {
      return pack_tcp_message(send_buf, MsgType::kModifyReject,
                              std::span{payload_buf_.data(), mr_len});
    }
    return 0;
  }
};

} // namespace mk::app
