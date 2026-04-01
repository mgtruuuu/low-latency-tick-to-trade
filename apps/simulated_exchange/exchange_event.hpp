/**
 * @file exchange_event.hpp
 * @brief Exchange event types for the event bus architecture.
 *
 * ExchangeEvent is a fixed-size POD struct representing any event
 * produced by ExchangeCore (order accepted, fill, cancel ack, etc.).
 * All event types share the same struct layout — unused fields are
 * zero-initialized. This avoids std::variant overhead and keeps the
 * event buffer zero-allocation.
 *
 * Event bus pattern:
 *   ExchangeCore emits events into a fixed buffer. Consumers
 *   (Gateway, Publisher) read from the buffer independently via
 *   shared memory queues. Adding a consumer requires zero changes
 *   to ExchangeCore.
 */

#pragma once

#include "shared/protocol.hpp"

#include "algo/trading_types.hpp"

#include <cstdint>

namespace mk::app {

/// Type tag for ExchangeEvent — determines which fields are meaningful.
enum class EventType : std::uint8_t {
  kOrderAccepted,   ///< Order rested or fully filled (OrderAck).
  kOrderRejected,   ///< Order rejected (invalid symbol, book full, etc.).
  kFill,            ///< Trade execution (FillReport).
  kCancelAck,       ///< Cancel succeeded.
  kCancelRejected,  ///< Cancel failed (order not found, etc.).
  kModifyAck,       ///< Modify succeeded (new exchange_order_id assigned).
  kModifyRejected,  ///< Modify failed (order not found, etc.).
  kBBOUpdate,       ///< Best bid/ask changed after a fill.
  kSessionAssigned, ///< Session ID assigned (response to client connect).
};

/// A single exchange event — union-free, fixed-size POD.
///
/// All event types use the same struct. Fields not relevant to a given
/// EventType are left at their default (zero) values. This keeps the
/// event buffer homogeneous and avoids variant/union complexity.
///
/// Field usage by event type:
///   kOrderAccepted:  session_id, client_order_id, exchange_order_id, send_ts
///   kOrderRejected:  session_id, client_order_id, reason, send_ts
///   kFill:           session_id, client_order_id, exchange_order_id,
///                    symbol_id, side, price, qty, remaining_qty, send_ts
///   kCancelAck:      session_id, client_order_id, send_ts
///   kCancelRejected: session_id, client_order_id, reason, send_ts
///   kModifyAck:      session_id, client_order_id, exchange_order_id, send_ts
///   kModifyRejected: session_id, client_order_id, reason, send_ts
///   kBBOUpdate:      symbol_id, side, price, qty (BBO snapshot post-fill)
///   kSessionAssigned: session_id (client_order_id set by engine but unused)
struct ExchangeEvent {
  EventType type{};
  std::uint16_t session_id{0};
  std::uint32_t request_seq{0}; ///< Copied from OrderRequest (IPC batch ID).
  std::uint64_t client_order_id{0};
  std::uint64_t exchange_order_id{0};
  std::uint32_t symbol_id{0};
  algo::Side side{};
  algo::Price price{0};
  algo::Qty qty{0};
  algo::Qty remaining_qty{0};
  std::int64_t send_ts{0};
  RejectReason reason{};
};

} // namespace mk::app
