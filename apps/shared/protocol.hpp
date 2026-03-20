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

inline constexpr std::uint16_t kProtocolVersion = 1;

/// Message type discriminator for the TLV header (message_codec.hpp).
/// Values 1-99 reserved for this protocol.
enum class MsgType : std::uint16_t {
  kMarketDataUpdate = 1, // Exchange -> Pipeline (UDP)
  kNewOrder = 2,         // Pipeline -> Exchange (TCP)
  kCancelOrder = 3,      // Pipeline -> Exchange (TCP)
  kOrderAck = 4,         // Exchange -> Pipeline (TCP)
  kOrderReject = 5,      // Exchange -> Pipeline (TCP)
  kFillReport = 6,       // Exchange -> Pipeline (TCP)
  kCancelAck = 7,        // Exchange -> Pipeline (TCP)
  kCancelReject = 8,     // Exchange -> Pipeline (TCP)
  kModifyOrder = 9,      // Pipeline -> Exchange (TCP)
  kModifyAck = 10,       // Exchange -> Pipeline (TCP)
  kModifyReject = 11,    // Exchange -> Pipeline (TCP)
  kHeartbeat = 12,       // Pipeline -> Exchange (TCP)
  kHeartbeatAck = 13,    // Exchange -> Pipeline (TCP)
};

/// Order rejection reason codes.
enum class RejectReason : std::uint8_t {
  kUnknown = 0,
  kDuplicateOrderId = 1,
  kBookFull = 2,
  kInvalidPrice = 3,
  kInvalidQty = 4,
  kOrderNotFound = 5,
};

// ======================================================================
// Market Data Update (UDP datagram payload)
// ======================================================================
// Wire layout (36 bytes, all big-endian):
//   [seq_num:8][symbol_id:4][side:1][padding:3][price:8][qty:4][exchange_ts:8]
//
// No TLV header on UDP -- one datagram = one message.
// Padding after side ensures price is 8-byte aligned on the wire for
// clarity, though WireReader/WireWriter use memcpy (no alignment needed).

struct MarketDataUpdate {
  std::uint64_t seq_num{0};    // Monotonic sequence for gap detection
  std::uint32_t symbol_id{0};  // Instrument identifier
  algo::Side side{};           // Bid or Ask
  algo::Price price{0};        // Fixed-point tick price
  algo::Qty qty{0};            // Quantity at this level
  std::int64_t exchange_ts{0}; // Exchange timestamp (monotonic nanos)
};

inline constexpr std::size_t kMarketDataWireSize =
    sizeof(std::uint64_t) + // seq_num
    sizeof(std::uint32_t) + // symbol_id
    sizeof(std::uint8_t) +  // side
    3 +                     // padding (aligns price on wire)
    sizeof(std::int64_t) +  // price
    sizeof(std::uint32_t) + // qty
    sizeof(std::int64_t);   // exchange_ts
static_assert(kMarketDataWireSize == 36);

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
