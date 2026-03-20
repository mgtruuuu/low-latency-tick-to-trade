/**
 * @file protocol_codec.hpp
 * @brief Serialize/deserialize functions for the tick-to-trade wire protocol.
 *
 * All functions use the same pattern: single upfront bounds check against
 * the known wire size, then unchecked stores/loads via sys/endian.hpp.
 * This eliminates per-field branching — one branch per message instead of
 * N branches per N fields.
 *
 * This is the standard HFT codec pattern: when all message sizes are known
 * at compile time, there's no reason to pay per-field bounds check overhead.
 * WireWriter/WireReader are still useful for variable-length or unknown-size
 * protocols, but every message in this protocol has a fixed wire size.
 *
 * TCP messages are wrapped in the TLV format from message_codec.hpp:
 *   [magic:4][version:2][msg_type:2][payload_len:4][flags:4][payload:N]
 * The serialize_* functions here produce the payload portion. The caller
 * wraps it with pack_message() for TCP, or sends it raw for UDP.
 */

#pragma once

#include "shared/protocol.hpp"

#include "net/message_codec.hpp"
#include "sys/endian.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace mk::app {

// Wire codec type assumptions — if any of these change in algo/types.hpp,
// every store_be*/load_be* call and wire size constant in this file must
// be updated to match.
static_assert(sizeof(algo::Price) == 8, "Wire codec assumes Price is 8 bytes");
static_assert(sizeof(algo::Qty) == 4, "Wire codec assumes Qty is 4 bytes");
static_assert(sizeof(algo::Side) == 1, "Wire codec assumes Side is 1 byte");

// ======================================================================
// UDP Market Data (36 bytes)
// Wire layout:
// [seq_num:8][symbol_id:4][side:1][pad:3][price:8][qty:4][exchange_ts:8]
// ======================================================================

/// Serialize a MarketDataUpdate into buf (exactly kMarketDataWireSize bytes).
/// @return Bytes written, or 0 if buf is too small.
[[nodiscard]] inline std::size_t
serialize_market_data(std::span<std::byte> buf,
                      const MarketDataUpdate &md) noexcept {
  if (buf.size() < kMarketDataWireSize) [[unlikely]] {
    return 0;
  }

  auto *p = buf.data();
  sys::store_be64(p + 0, md.seq_num);
  sys::store_be32(p + 8, md.symbol_id);
  std::memset(p + 12, 0, 4); // side + 3 bytes padding
  p[12] = static_cast<std::byte>(md.side);
  sys::store_be64(p + 16, static_cast<std::uint64_t>(md.price));
  sys::store_be32(p + 24, md.qty);
  sys::store_be64(p + 28, static_cast<std::uint64_t>(md.exchange_ts));
  return kMarketDataWireSize;
}

/// Deserialize a MarketDataUpdate from buf into out.
/// @return true on success, false if buf is too small (out unchanged).
[[nodiscard]] inline bool
deserialize_market_data(std::span<const std::byte> buf,
                        MarketDataUpdate &out) noexcept {
  if (buf.size() < kMarketDataWireSize) [[unlikely]] {
    return false;
  }

  const auto *p = buf.data();
  out.seq_num = sys::load_be64(p + 0);
  out.symbol_id = sys::load_be32(p + 8);
  out.side = static_cast<algo::Side>(p[12]);
  out.price = static_cast<algo::Price>(sys::load_be64(p + 16));
  out.qty = sys::load_be32(p + 24);
  out.exchange_ts = static_cast<std::int64_t>(sys::load_be64(p + 28));
  return true;
}

// ======================================================================
// NewOrder (33 bytes)
// Wire layout:
// [client_order_id:8][symbol_id:4][side:1][price:8][qty:4][send_ts:8]
// ======================================================================

/// Serialize NewOrder payload into buf.
/// @return Bytes written, or 0 if buf is too small.
[[nodiscard]] inline std::size_t
serialize_new_order(std::span<std::byte> buf, const NewOrder &o) noexcept {
  if (buf.size() < kNewOrderWireSize) [[unlikely]] {
    return 0;
  }

  auto *p = buf.data();
  sys::store_be64(p + 0, o.client_order_id);
  sys::store_be32(p + 8, o.symbol_id);
  p[12] = static_cast<std::byte>(o.side);
  sys::store_be64(p + 13, static_cast<std::uint64_t>(o.price));
  sys::store_be32(p + 21, o.qty);
  sys::store_be64(p + 25, static_cast<std::uint64_t>(o.send_ts));
  return kNewOrderWireSize;
}

/// Deserialize NewOrder payload into out.
/// @return true on success, false if buf is too small (out unchanged).
[[nodiscard]] inline bool deserialize_new_order(std::span<const std::byte> buf,
                                                NewOrder &out) noexcept {
  if (buf.size() < kNewOrderWireSize) [[unlikely]] {
    return false;
  }

  const auto *p = buf.data();
  out.client_order_id = sys::load_be64(p + 0);
  out.symbol_id = sys::load_be32(p + 8);
  out.side = static_cast<algo::Side>(p[12]);
  out.price = static_cast<algo::Price>(sys::load_be64(p + 13));
  out.qty = sys::load_be32(p + 21);
  out.send_ts = static_cast<std::int64_t>(sys::load_be64(p + 25));
  return true;
}

// ======================================================================
// CancelOrder (20 bytes)
// Wire layout: [client_order_id:8][symbol_id:4][send_ts:8]
// ======================================================================

[[nodiscard]] inline std::size_t
serialize_cancel_order(std::span<std::byte> buf,
                       const CancelOrder &c) noexcept {
  if (buf.size() < kCancelOrderWireSize) [[unlikely]] {
    return 0;
  }

  auto *p = buf.data();
  sys::store_be64(p + 0, c.client_order_id);
  sys::store_be32(p + 8, c.symbol_id);
  sys::store_be64(p + 12, static_cast<std::uint64_t>(c.send_ts));
  return kCancelOrderWireSize;
}

[[nodiscard]] inline bool
deserialize_cancel_order(std::span<const std::byte> buf,
                         CancelOrder &out) noexcept {
  if (buf.size() < kCancelOrderWireSize) [[unlikely]] {
    return false;
  }

  const auto *p = buf.data();
  out.client_order_id = sys::load_be64(p + 0);
  out.symbol_id = sys::load_be32(p + 8);
  out.send_ts = static_cast<std::int64_t>(sys::load_be64(p + 12));
  return true;
}

// ======================================================================
// OrderAck (24 bytes)
// Wire layout: [client_order_id:8][exchange_order_id:8][send_ts:8]
// ======================================================================

[[nodiscard]] inline std::size_t
serialize_order_ack(std::span<std::byte> buf, const OrderAck &a) noexcept {
  if (buf.size() < kOrderAckWireSize) [[unlikely]] {
    return 0;
  }

  auto *p = buf.data();
  sys::store_be64(p + 0, a.client_order_id);
  sys::store_be64(p + 8, a.exchange_order_id);
  sys::store_be64(p + 16, static_cast<std::uint64_t>(a.send_ts));
  return kOrderAckWireSize;
}

[[nodiscard]] inline bool deserialize_order_ack(std::span<const std::byte> buf,
                                                OrderAck &out) noexcept {
  if (buf.size() < kOrderAckWireSize) [[unlikely]] {
    return false;
  }

  const auto *p = buf.data();
  out.client_order_id = sys::load_be64(p + 0);
  out.exchange_order_id = sys::load_be64(p + 8);
  out.send_ts = static_cast<std::int64_t>(sys::load_be64(p + 16));
  return true;
}

// ======================================================================
// OrderReject (17 bytes)
// Wire layout: [client_order_id:8][reason:1][send_ts:8]
// ======================================================================

[[nodiscard]] inline std::size_t
serialize_order_reject(std::span<std::byte> buf,
                       const OrderReject &r_msg) noexcept {
  if (buf.size() < kOrderRejectWireSize) [[unlikely]] {
    return 0;
  }

  auto *p = buf.data();
  sys::store_be64(p + 0, r_msg.client_order_id);
  p[8] = static_cast<std::byte>(r_msg.reason);
  sys::store_be64(p + 9, static_cast<std::uint64_t>(r_msg.send_ts));
  return kOrderRejectWireSize;
}

[[nodiscard]] inline bool
deserialize_order_reject(std::span<const std::byte> buf,
                         OrderReject &out) noexcept {
  if (buf.size() < kOrderRejectWireSize) [[unlikely]] {
    return false;
  }

  const auto *p = buf.data();
  out.client_order_id = sys::load_be64(p + 0);
  out.reason = static_cast<RejectReason>(p[8]);
  out.send_ts = static_cast<std::int64_t>(sys::load_be64(p + 9));
  return true;
}

// ======================================================================
// FillReport (40 bytes)
// Wire layout: [client_order_id:8][exchange_order_id:8][fill_price:8]
//              [fill_qty:4][remaining_qty:4][send_ts:8]
// ======================================================================

[[nodiscard]] inline std::size_t
serialize_fill_report(std::span<std::byte> buf, const FillReport &f) noexcept {
  if (buf.size() < kFillReportWireSize) [[unlikely]] {
    return 0;
  }

  auto *p = buf.data();
  sys::store_be64(p + 0, f.client_order_id);
  sys::store_be64(p + 8, f.exchange_order_id);
  sys::store_be64(p + 16, static_cast<std::uint64_t>(f.fill_price));
  sys::store_be32(p + 24, f.fill_qty);
  sys::store_be32(p + 28, f.remaining_qty);
  sys::store_be64(p + 32, static_cast<std::uint64_t>(f.send_ts));
  return kFillReportWireSize;
}

[[nodiscard]] inline bool
deserialize_fill_report(std::span<const std::byte> buf,
                        FillReport &out) noexcept {
  if (buf.size() < kFillReportWireSize) [[unlikely]] {
    return false;
  }

  const auto *p = buf.data();
  out.client_order_id = sys::load_be64(p + 0);
  out.exchange_order_id = sys::load_be64(p + 8);
  out.fill_price = static_cast<algo::Price>(sys::load_be64(p + 16));
  out.fill_qty = sys::load_be32(p + 24);
  out.remaining_qty = sys::load_be32(p + 28);
  out.send_ts = static_cast<std::int64_t>(sys::load_be64(p + 32));
  return true;
}

// ======================================================================
// CancelAck (16 bytes)
// Wire layout: [client_order_id:8][send_ts:8]
// ======================================================================

[[nodiscard]] inline std::size_t
serialize_cancel_ack(std::span<std::byte> buf, const CancelAck &a) noexcept {
  if (buf.size() < kCancelAckWireSize) [[unlikely]] {
    return 0;
  }

  auto *p = buf.data();
  sys::store_be64(p + 0, a.client_order_id);
  sys::store_be64(p + 8, static_cast<std::uint64_t>(a.send_ts));
  return kCancelAckWireSize;
}

[[nodiscard]] inline bool deserialize_cancel_ack(std::span<const std::byte> buf,
                                                 CancelAck &out) noexcept {
  if (buf.size() < kCancelAckWireSize) [[unlikely]] {
    return false;
  }

  const auto *p = buf.data();
  out.client_order_id = sys::load_be64(p + 0);
  out.send_ts = static_cast<std::int64_t>(sys::load_be64(p + 8));
  return true;
}

// ======================================================================
// CancelReject (17 bytes)
// Wire layout: [client_order_id:8][reason:1][send_ts:8]
// ======================================================================

[[nodiscard]] inline std::size_t
serialize_cancel_reject(std::span<std::byte> buf,
                        const CancelReject &cr) noexcept {
  if (buf.size() < kCancelRejectWireSize) [[unlikely]] {
    return 0;
  }

  auto *p = buf.data();
  sys::store_be64(p + 0, cr.client_order_id);
  p[8] = static_cast<std::byte>(cr.reason);
  sys::store_be64(p + 9, static_cast<std::uint64_t>(cr.send_ts));
  return kCancelRejectWireSize;
}

[[nodiscard]] inline bool
deserialize_cancel_reject(std::span<const std::byte> buf,
                          CancelReject &out) noexcept {
  if (buf.size() < kCancelRejectWireSize) [[unlikely]] {
    return false;
  }

  const auto *p = buf.data();
  out.client_order_id = sys::load_be64(p + 0);
  out.reason = static_cast<RejectReason>(p[8]);
  out.send_ts = static_cast<std::int64_t>(sys::load_be64(p + 9));
  return true;
}

// ======================================================================
// ModifyOrder (32 bytes)
// Wire layout: [client_order_id:8][symbol_id:4][new_price:8][new_qty:4][send_ts:8]
// ======================================================================

[[nodiscard]] inline std::size_t
serialize_modify_order(std::span<std::byte> buf,
                       const ModifyOrder &mo) noexcept {
  if (buf.size() < kModifyOrderWireSize) [[unlikely]] {
    return 0;
  }

  auto *p = buf.data();
  sys::store_be64(p + 0, mo.client_order_id);
  sys::store_be32(p + 8, mo.symbol_id);
  sys::store_be64(p + 12, static_cast<std::uint64_t>(mo.new_price));
  sys::store_be32(p + 20, static_cast<std::uint32_t>(mo.new_qty));
  sys::store_be64(p + 24, static_cast<std::uint64_t>(mo.send_ts));
  return kModifyOrderWireSize;
}

[[nodiscard]] inline bool
deserialize_modify_order(std::span<const std::byte> buf,
                         ModifyOrder &out) noexcept {
  if (buf.size() < kModifyOrderWireSize) [[unlikely]] {
    return false;
  }

  const auto *p = buf.data();
  out.client_order_id = sys::load_be64(p + 0);
  out.symbol_id = sys::load_be32(p + 8);
  out.new_price = static_cast<algo::Price>(sys::load_be64(p + 12));
  out.new_qty = static_cast<algo::Qty>(sys::load_be32(p + 20));
  out.send_ts = static_cast<std::int64_t>(sys::load_be64(p + 24));
  return true;
}

// ======================================================================
// ModifyAck (24 bytes)
// Wire layout: [client_order_id:8][new_exchange_order_id:8][send_ts:8]
// ======================================================================

[[nodiscard]] inline std::size_t
serialize_modify_ack(std::span<std::byte> buf,
                     const ModifyAck &ma) noexcept {
  if (buf.size() < kModifyAckWireSize) [[unlikely]] {
    return 0;
  }

  auto *p = buf.data();
  sys::store_be64(p + 0, ma.client_order_id);
  sys::store_be64(p + 8, ma.new_exchange_order_id);
  sys::store_be64(p + 16, static_cast<std::uint64_t>(ma.send_ts));
  return kModifyAckWireSize;
}

[[nodiscard]] inline bool
deserialize_modify_ack(std::span<const std::byte> buf,
                       ModifyAck &out) noexcept {
  if (buf.size() < kModifyAckWireSize) [[unlikely]] {
    return false;
  }

  const auto *p = buf.data();
  out.client_order_id = sys::load_be64(p + 0);
  out.new_exchange_order_id = sys::load_be64(p + 8);
  out.send_ts = static_cast<std::int64_t>(sys::load_be64(p + 16));
  return true;
}

// ======================================================================
// ModifyReject (17 bytes)
// Wire layout: [client_order_id:8][reason:1][send_ts:8]
// ======================================================================

[[nodiscard]] inline std::size_t
serialize_modify_reject(std::span<std::byte> buf,
                        const ModifyReject &mr) noexcept {
  if (buf.size() < kModifyRejectWireSize) [[unlikely]] {
    return 0;
  }

  auto *p = buf.data();
  sys::store_be64(p + 0, mr.client_order_id);
  p[8] = static_cast<std::byte>(mr.reason);
  sys::store_be64(p + 9, static_cast<std::uint64_t>(mr.send_ts));
  return kModifyRejectWireSize;
}

[[nodiscard]] inline bool
deserialize_modify_reject(std::span<const std::byte> buf,
                          ModifyReject &out) noexcept {
  if (buf.size() < kModifyRejectWireSize) [[unlikely]] {
    return false;
  }

  const auto *p = buf.data();
  out.client_order_id = sys::load_be64(p + 0);
  out.reason = static_cast<RejectReason>(p[8]);
  out.send_ts = static_cast<std::int64_t>(sys::load_be64(p + 9));
  return true;
}

// ======================================================================
// TLV Helpers — wrap payload in message_codec.hpp TLV format for TCP
// ======================================================================

/// Pack a typed message into TLV format for TCP transmission.
/// @param out Output buffer (must fit kMessageHeaderSize + payload_size).
/// @param msg_type Message type discriminator.
/// @param payload Serialized payload bytes.
/// @return Total bytes written (header + payload), or 0 on overflow.
[[nodiscard]] inline std::size_t
pack_tcp_message(std::span<std::byte> out, MsgType msg_type,
                 std::span<const std::byte> payload) noexcept {
  return net::pack_message(out, kProtocolVersion,
                           static_cast<std::uint16_t>(msg_type),
                           0, // flags
                           payload);
}

} // namespace mk::app
