/**
 * @file protocol.hpp
 * @brief Wire protocol definitions shared between the trading pipeline and
 *        simulated exchange.
 *
 * Defines message types, structs, and wire sizes for the tick-to-trade
 * communication protocol. Two channels:
 *
 *   1. UDP multicast (exchange -> pipeline): Market data updates.
 *      One datagram = one message. No framing needed.
 *
 *   2. TCP (bidirectional): Order entry and responses.
 *      Uses message_codec.hpp TLV format for framing and msg_type
 *      discrimination.
 *
 * All wire formats use big-endian (network byte order).
 * Serialization/deserialization functions are in protocol_codec.hpp.
 */

#pragma once

#include "algo/trading_types.hpp" // Price, OrderId, Qty, Side

#include <cstdint>

namespace mk::app {

// ======================================================================
// Protocol constants
// ======================================================================

/// TCP protocol version stamped in every TLV header (message_codec.hpp).
/// Receivers MUST reject frames whose header.version != kProtocolVersion;
/// see verify_protocol_version() below. Bump this constant whenever any
/// on-the-wire change is made — new MsgType, reordered MsgType, new field
/// in any payload, payload size change, etc. — so a peer running an older
/// build fails closed instead of silently misinterpreting bytes.
///
/// UDP exception: the version field lives only in the TCP TLV header.
/// The UDP MarketData datagram has no transmitted version; the current
/// 34B layout is protected only by exact datagram-size validation in
/// deserialize_market_data(). A future same-size UDP schema change
/// would need its own discriminator/versioned schema.
///
/// History:
///   1 — initial protocol.
///   2 — UDP MarketData payload compacted 36B->34B (size-rejected on
///       mismatch). TCP MsgType numeric IDs unchanged from v1; explicit
///       `= N` assignments + a per-entry static_assert block were added
///       in this version to freeze the wire contract against future
///       accidental renumbering. v2 receivers reject mismatched TCP
///       input; retained v1 MsgType IDs keep any request processed by
///       a v1 peer semantically compatible. A v1 peer has no version
///       check of its own — fail-closed behavior on a mixed-version
///       pair is therefore one-sided (v2-side rejection only); the
///       cross-version safety guarantee depends on the ID-preservation
///       contract above.
inline constexpr std::uint16_t kProtocolVersion = 2;

/// Message type discriminator for the TLV header (message_codec.hpp).
/// Values 1-99 reserved for this protocol.
///
/// Source layout follows numeric ID order (1..13). Lifecycle grouping
/// (NewOrder request/response, Cancel request/response, Modify
/// request/response, etc.) is indicated by per-line inline tags rather
/// than by source blocks — a request and its responses do not sit next
/// to each other in source because the IDs were assigned in historical
/// insertion order and that order is the wire contract.
///
/// The static_assert block below is the authoritative freeze for every
/// entry's numeric value; the inline comments are a readability aid only.
///
/// Editing rules:
///   - To ADD a new message type: append it at the end with the next
///     unused integer, and add a matching static_assert. Never reuse a
///     retired ID — a real external exchange protocol (FIX, ITCH) is
///     append-only for the same reason.
///   - To CHANGE an existing message's wire ID: bump kProtocolVersion
///     above so peers fail closed on mismatch. Note that an old peer
///     can still mis-handle requests whose IDs happen to overlap before
///     it sees its first response — true safety against that case
///     requires a connect-time handshake (currently not implemented).
///   - The static_assert block must list every entry; a renumber that
///     escapes the asserts is a silent wire-break and the asserts are
///     the only build-time tripwire against it.
enum class MsgType : std::uint16_t {
  kMarketDataUpdate = 1, // [UDP] Exchange -> Pipeline (standalone datagram)
  kNewOrder = 2,         // [TCP] Pipeline -> Exchange (NewOrder request)
  kCancelOrder = 3,      // [TCP] Pipeline -> Exchange (Cancel request)
  kOrderAck = 4,         // [TCP] Exchange -> Pipeline (NewOrder accepted)
  kOrderReject = 5,      // [TCP] Exchange -> Pipeline (NewOrder refused)
  kFillReport = 6,       // [TCP] Exchange -> Pipeline (execution event,
                         //       from NewOrder + Modify paths — see
                         //       exchange_core::modify_order)
  kCancelAck = 7,        // [TCP] Exchange -> Pipeline (Cancel accepted)
  kCancelReject = 8,     // [TCP] Exchange -> Pipeline (Cancel refused)
  kModifyOrder = 9,      // [TCP] Pipeline -> Exchange (Modify request)
  kModifyAck = 10,       // [TCP] Exchange -> Pipeline (Modify accepted)
  kModifyReject = 11,    // [TCP] Exchange -> Pipeline (Modify refused)
  kHeartbeat = 12,       // [TCP] Pipeline -> Exchange (ping)
  kHeartbeatAck = 13,    // [TCP] Exchange -> Pipeline (pong)
};

// Wire-ID freeze. Every MsgType entry must appear here. A renumber or
// reorder that escapes these asserts is a silent wire-break — the
// asserts are the only build-time guarantee that producer and consumer
// agree on every numeric value. Pair every change with a corresponding
// kProtocolVersion bump.
static_assert(static_cast<std::uint16_t>(MsgType::kMarketDataUpdate) == 1);
static_assert(static_cast<std::uint16_t>(MsgType::kNewOrder) == 2);
static_assert(static_cast<std::uint16_t>(MsgType::kCancelOrder) == 3);
static_assert(static_cast<std::uint16_t>(MsgType::kOrderAck) == 4);
static_assert(static_cast<std::uint16_t>(MsgType::kOrderReject) == 5);
static_assert(static_cast<std::uint16_t>(MsgType::kFillReport) == 6);
static_assert(static_cast<std::uint16_t>(MsgType::kCancelAck) == 7);
static_assert(static_cast<std::uint16_t>(MsgType::kCancelReject) == 8);
static_assert(static_cast<std::uint16_t>(MsgType::kModifyOrder) == 9);
static_assert(static_cast<std::uint16_t>(MsgType::kModifyAck) == 10);
static_assert(static_cast<std::uint16_t>(MsgType::kModifyReject) == 11);
static_assert(static_cast<std::uint16_t>(MsgType::kHeartbeat) == 12);
static_assert(static_cast<std::uint16_t>(MsgType::kHeartbeatAck) == 13);

/// Verify a TLV header's protocol version against the current build.
/// Returns true if the version matches kProtocolVersion; false if the
/// peer is speaking a different version of the protocol and the frame
/// must be rejected.
///
/// Mismatch handling is the caller's responsibility — the canonical
/// fail-closed behavior in this codebase is: log the mismatch with both
/// versions, drop the frame, and (for the trading-pipeline side) signal
/// the kill switch since further responses cannot be trusted. The
/// exchange-gateway side rejects the frame; the TCP framer simply
/// advances past it on the next loop iteration.
[[nodiscard]] constexpr bool
verify_protocol_version(std::uint16_t header_version) noexcept {
  return header_version == kProtocolVersion;
}

/// Order rejection reason codes.
enum class RejectReason : std::uint8_t {
  kUnknown = 0,
  kDuplicateOrderId = 1,
  kBookFull = 2,
  kInvalidPrice = 3,
  kInvalidQty = 4,
  kOrderNotFound = 5,
  kInvalidOrderId = 6, ///< client_order_id exceeds 48-bit composite key range.
  kUnknownSymbol = 7,  ///< symbol_id not registered in the exchange.
  kThrottled = 8,      ///< Gateway overloaded (request queue full).
};

// ======================================================================
// Market Data Update (UDP datagram payload)
// ======================================================================
// Wire layout (34 bytes, all big-endian):
//   [seq_num:8][symbol_id:4][md_msg_type:1][side:1][price:8][qty:4][exchange_ts:8]
//
// No TLV header on UDP -- one datagram = one message.
// No padding on the wire -- the codec uses memcpy so wire alignment is
// not required. Future schema evolution would use a versioned schema
// (e.g., SBE) rather than reserved bytes.

/// Market data message type (discriminates BBO update vs trade on UDP).
enum class MdMsgType : std::uint8_t {
  kBBOUpdate = 0, ///< Best bid or ask price/qty update.
  kTrade = 1,     ///< Trade execution (price + qty).
};

struct MarketDataUpdate {
  std::uint64_t seq_num{0};              // Monotonic sequence for gap detection
  std::uint32_t symbol_id{0};            // Instrument identifier
  MdMsgType md_msg_type{MdMsgType::kBBOUpdate}; // BBO or Trade
  algo::Side side{};                     // Bid or Ask
  algo::Price price{0};                  // Fixed-point tick price
  algo::Qty qty{0};                      // Quantity at this level
  std::int64_t exchange_ts{0};           // Exchange timestamp (monotonic nanos)
};

// clang-format off
inline constexpr std::size_t kMarketDataWireSize =
    sizeof(std::uint64_t) + // seq_num       (8)
    sizeof(std::uint32_t) + // symbol_id     (4)
    sizeof(std::uint8_t) +  // md_msg_type   (1)
    sizeof(std::uint8_t) +  // side          (1)
    sizeof(std::int64_t) +  // price         (8)
    sizeof(std::uint32_t) + // qty           (4)
    sizeof(std::int64_t);   // exchange_ts   (8)
// clang-format on
static_assert(kMarketDataWireSize == 34);

// ======================================================================
// New Order (TCP, wrapped in TLV message_codec)
// ======================================================================
// Payload layout (33 bytes):
//   [client_order_id:8][symbol_id:4][side:1][price:8][qty:4][send_ts:8]
//
// send_ts is the pipeline's monotonic_nanos() at order creation,
// echoed back in OrderAck for round-trip latency measurement.

struct NewOrder {
  std::uint64_t client_order_id{0};
  std::uint32_t symbol_id{0};
  algo::Side side{};
  algo::Price price{0};
  algo::Qty qty{0};
  std::int64_t send_ts{0}; // Pipeline's timestamp at send time
};

inline constexpr std::size_t kNewOrderWireSize =
    sizeof(std::uint64_t) + // client_order_id
    sizeof(std::uint32_t) + // symbol_id
    sizeof(std::uint8_t) +  // side
    sizeof(std::int64_t) +  // price
    sizeof(std::uint32_t) + // qty
    sizeof(std::int64_t);   // send_ts
static_assert(kNewOrderWireSize == 33);

// ======================================================================
// Cancel Order (TCP, wrapped in TLV message_codec)
// ======================================================================
// Payload layout (16 bytes):
//   [client_order_id:8][send_ts:8]

struct CancelOrder {
  std::uint64_t client_order_id{0};
  std::uint32_t symbol_id{0}; // Instrument ID (for multi-symbol routing)
  std::int64_t send_ts{0};
};

inline constexpr std::size_t kCancelOrderWireSize =
    sizeof(std::uint64_t) + // client_order_id
    sizeof(std::uint32_t) + // symbol_id
    sizeof(std::int64_t);   // send_ts
static_assert(kCancelOrderWireSize == 20);

// ======================================================================
// Order Ack (TCP, wrapped in TLV message_codec)
// ======================================================================
// Payload layout (24 bytes):
//   [client_order_id:8][exchange_order_id:8][send_ts:8]
//
// send_ts is echoed from NewOrder -- pipeline computes round-trip
// by subtracting from current timestamp.

struct OrderAck {
  std::uint64_t client_order_id{0};
  std::uint64_t exchange_order_id{0};
  std::int64_t send_ts{0}; // Echoed from NewOrder
};

inline constexpr std::size_t kOrderAckWireSize =
    sizeof(std::uint64_t) + // client_order_id
    sizeof(std::uint64_t) + // exchange_order_id
    sizeof(std::int64_t);   // send_ts
static_assert(kOrderAckWireSize == 24);

// ======================================================================
// Order Reject (TCP, wrapped in TLV message_codec)
// ======================================================================
// Payload layout (17 bytes):
//   [client_order_id:8][reason:1][send_ts:8]

struct OrderReject {
  std::uint64_t client_order_id{0};
  RejectReason reason{};
  std::int64_t send_ts{0}; // Echoed from NewOrder
};

inline constexpr std::size_t kOrderRejectWireSize =
    sizeof(std::uint64_t) + // client_order_id
    sizeof(std::uint8_t) +  // reason
    sizeof(std::int64_t);   // send_ts
static_assert(kOrderRejectWireSize == 17);

// ======================================================================
// Fill Report (TCP, wrapped in TLV message_codec)
// ======================================================================
// Payload layout (40 bytes):
//   [client_order_id:8][exchange_order_id:8][fill_price:8][fill_qty:4]
//   [remaining_qty:4][send_ts:8]

struct FillReport {
  std::uint64_t client_order_id{0};
  std::uint64_t exchange_order_id{0};
  algo::Price fill_price{0};
  algo::Qty fill_qty{0};
  algo::Qty remaining_qty{0};
  std::int64_t send_ts{0}; // Echoed from NewOrder
};

inline constexpr std::size_t kFillReportWireSize =
    sizeof(std::uint64_t) + // client_order_id
    sizeof(std::uint64_t) + // exchange_order_id
    sizeof(std::int64_t) +  // fill_price
    sizeof(std::uint32_t) + // fill_qty
    sizeof(std::uint32_t) + // remaining_qty
    sizeof(std::int64_t);   // send_ts
static_assert(kFillReportWireSize == 40);

// ======================================================================
// Cancel Ack (TCP, wrapped in TLV message_codec)
// ======================================================================
// Payload layout (16 bytes):
//   [client_order_id:8][send_ts:8]

struct CancelAck {
  std::uint64_t client_order_id{0};
  std::int64_t send_ts{0}; // Echoed from CancelOrder
};

inline constexpr std::size_t kCancelAckWireSize =
    sizeof(std::uint64_t) + // client_order_id
    sizeof(std::int64_t);   // send_ts
static_assert(kCancelAckWireSize == 16);

// ======================================================================
// Cancel Reject (TCP, wrapped in TLV message_codec)
// ======================================================================
// Payload layout (17 bytes):
//   [client_order_id:8][reason:1][send_ts:8]

struct CancelReject {
  std::uint64_t client_order_id{0};
  RejectReason reason{};
  std::int64_t send_ts{0}; // Echoed from CancelOrder
};

inline constexpr std::size_t kCancelRejectWireSize =
    sizeof(std::uint64_t) + // client_order_id
    sizeof(std::uint8_t) +  // reason
    sizeof(std::int64_t);   // send_ts
static_assert(kCancelRejectWireSize == 17);

// ======================================================================
// Modify Order (TCP, wrapped in TLV message_codec)
// ======================================================================
// Payload layout (32 bytes):
//   [client_order_id:8][symbol_id:4][new_price:8][new_qty:4][send_ts:8]
//
// Modifies a resting order's price and/or quantity. Internally implemented
// as cancel + re-add (loses time priority), matching FIX
// OrderCancelReplaceRequest (MsgType=G) and ITCH Order Replace (Type U).

struct ModifyOrder {
  std::uint64_t client_order_id{0};
  std::uint32_t symbol_id{0};
  algo::Price new_price{0};
  algo::Qty new_qty{0};
  std::int64_t send_ts{0};
};

inline constexpr std::size_t kModifyOrderWireSize =
    sizeof(std::uint64_t) + // client_order_id
    sizeof(std::uint32_t) + // symbol_id
    sizeof(std::int64_t) +  // new_price
    sizeof(std::uint32_t) + // new_qty
    sizeof(std::int64_t);   // send_ts
static_assert(kModifyOrderWireSize == 32);

// ======================================================================
// Modify Ack (TCP, wrapped in TLV message_codec)
// ======================================================================
// Payload layout (24 bytes):
//   [client_order_id:8][new_exchange_order_id:8][send_ts:8]

struct ModifyAck {
  std::uint64_t client_order_id{0};
  std::uint64_t new_exchange_order_id{0};
  std::int64_t send_ts{0};
};

inline constexpr std::size_t kModifyAckWireSize =
    sizeof(std::uint64_t) + // client_order_id
    sizeof(std::uint64_t) + // new_exchange_order_id
    sizeof(std::int64_t);   // send_ts
static_assert(kModifyAckWireSize == 24);

// ======================================================================
// Modify Reject (TCP, wrapped in TLV message_codec)
// ======================================================================
// Payload layout (17 bytes):
//   [client_order_id:8][reason:1][send_ts:8]

struct ModifyReject {
  std::uint64_t client_order_id{0};
  RejectReason reason{};
  std::int64_t send_ts{0};
};

inline constexpr std::size_t kModifyRejectWireSize =
    sizeof(std::uint64_t) + // client_order_id
    sizeof(std::uint8_t) +  // reason
    sizeof(std::int64_t);   // send_ts
static_assert(kModifyRejectWireSize == 17);

} // namespace mk::app
