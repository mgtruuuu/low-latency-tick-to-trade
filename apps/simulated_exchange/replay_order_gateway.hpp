/**
 * @file replay_order_gateway.hpp
 * @brief Simplified order gateway for replay mode — immediate fill, no
 *        matching engine.
 *
 * In replay mode the exchange publishes pre-recorded market data. There are
 * no resting orders in the book (the data is read-only), so running the
 * matching engine is meaningless. Instead, every order is immediately
 * acknowledged and fully filled at the order's requested price.
 *
 * This tests the pipeline's full response path (ack handling, fill handling,
 * position tracking, RTT measurement) without the complexity of maintaining
 * a synthetic order book.
 */

#pragma once

#include "shared/protocol.hpp"
#include "shared/protocol_codec.hpp"

#include "net/message_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mk::app {

class ReplayOrderGateway {
public:
  // NewOrder in replay mode returns exactly:
  //   1 OrderAck + 1 FillReport.
  static constexpr std::size_t kMaxResponseBytes =
      (net::kMessageHeaderSize + kOrderAckWireSize) +
      (net::kMessageHeaderSize + kFillReportWireSize);

  /// Process a complete TLV message. Writes response TLV frames into send_buf.
  /// @return Total bytes written to send_buf.
  [[nodiscard]] std::size_t on_message(std::span<const std::byte> raw_tlv,
                                       std::span<std::byte> send_buf) noexcept {
    net::ParsedMessageView msg;
    if (!net::unpack_message(raw_tlv, msg)) [[unlikely]] {
      return 0;
    }

    const auto msg_type = static_cast<MsgType>(msg.header.msg_type);
    const auto required_capacity = min_response_capacity(msg_type);
    if (send_buf.size() < required_capacity) [[unlikely]] {
      return 0;
    }

    switch (msg_type) {
    case MsgType::kNewOrder:
      return handle_new_order(msg.payload, send_buf);
    case MsgType::kCancelOrder:
      return handle_cancel(msg.payload, send_buf);
    case MsgType::kHeartbeat:
      // Reply with empty HeartbeatAck (TLV header only, no payload).
      return pack_tcp_message(send_buf, MsgType::kHeartbeatAck, {});
    default:
      return 0;
    }
  }

private:
  std::uint64_t next_exchange_id_{1};
  std::array<std::byte, 256> payload_buf_{};

  [[nodiscard]] static constexpr std::size_t
  min_response_capacity(MsgType msg_type) noexcept {
    constexpr std::size_t kAckWithNoPayload = net::kMessageHeaderSize;
    constexpr std::size_t kCancelRejectBytes =
        net::kMessageHeaderSize + kCancelRejectWireSize;
    switch (msg_type) {
    case MsgType::kNewOrder:
      return kMaxResponseBytes;
    case MsgType::kCancelOrder:
      return kCancelRejectBytes;
    case MsgType::kHeartbeat:
      return kAckWithNoPayload;
    default:
      return 0;
    }
  }

  std::size_t handle_new_order(std::span<const std::byte> payload,
                               std::span<std::byte> send_buf) noexcept {
    NewOrder order;
    if (!deserialize_new_order(payload, order)) [[unlikely]] {
      return 0;
    }

    std::size_t total = 0;
    const auto exchange_id = next_exchange_id_++;

    // 1. Immediate OrderAck.
    OrderAck ack;
    ack.client_order_id = order.client_order_id;
    ack.exchange_order_id = exchange_id;
    ack.send_ts = order.send_ts;

    const auto ack_len = serialize_order_ack(payload_buf_, ack);
    if (ack_len > 0) {
      total += pack_tcp_message(send_buf.subspan(total), MsgType::kOrderAck,
                                std::span{payload_buf_.data(), ack_len});
    }

    // 2. Immediate full fill at order price.
    FillReport fill;
    fill.client_order_id = order.client_order_id;
    fill.exchange_order_id = exchange_id;
    fill.fill_price = order.price;
    fill.fill_qty = order.qty;
    fill.remaining_qty = 0; // Fully filled
    fill.send_ts = order.send_ts;

    const auto fill_len = serialize_fill_report(payload_buf_, fill);
    if (fill_len > 0) {
      total += pack_tcp_message(send_buf.subspan(total), MsgType::kFillReport,
                                std::span{payload_buf_.data(), fill_len});
    }

    return total;
  }

  std::size_t handle_cancel(std::span<const std::byte> payload,
                            std::span<std::byte> send_buf) noexcept {
    CancelOrder cancel;
    if (!deserialize_cancel_order(payload, cancel)) [[unlikely]] {
      return 0;
    }

    // In replay mode, orders are immediately filled — nothing to cancel.
    // Respond with CancelReject.
    CancelReject cr;
    cr.client_order_id = cancel.client_order_id;
    cr.reason = RejectReason::kUnknown;
    cr.send_ts = cancel.send_ts;

    const auto cr_len = serialize_cancel_reject(payload_buf_, cr);
    if (cr_len > 0) {
      return pack_tcp_message(send_buf, MsgType::kCancelReject,
                              std::span{payload_buf_.data(), cr_len});
    }
    return 0;
  }
};

} // namespace mk::app
