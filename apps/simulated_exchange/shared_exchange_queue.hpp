/**
 * @file shared_exchange_queue.hpp
 * @brief App-specific types and layout for cross-process exchange IPC.
 *
 * Defines the application-layer IPC types for the multi-gateway exchange:
 *
 *   Gateway 0 ──request_queues[0]──► Engine ──response_queues[0]──► Gateway 0
 *   Gateway 1 ──request_queues[1]──► Engine ──response_queues[1]──► Gateway 1
 *   ...
 *                                    Engine ──md_event_queue─────► MD Publisher
 *
 * Each Gateway process serves exactly one client (industry standard).
 * The Engine round-robin polls all active request queues and routes
 * responses to the correct gateway via session_to_gateway[session_id].
 *
 * The underlying SPSC queue (FixedSPSCQueue) lives in libs/sys/memory/.
 * FixedSPSCQueue works in shared memory because all state (head, tail,
 * buffer) is inline in the struct and atomics are lock-free on x86-64.
 * This file provides:
 *   - OrderRequest / OrderRequestType: gateway → engine message types
 *   - SharedExchangeRegion: top-level shared memory layout
 *   - Queue type aliases with exchange-specific default capacity
 */

#pragma once

#include "exchange_event.hpp"

#include "algo/trading_types.hpp"
#include "sys/hardware_constants.hpp"
#include "sys/memory/fixed_spsc_queue.hpp"

#include <atomic>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace mk::app {

// Default POSIX shared memory name (must start with /, no other / allowed).
inline constexpr std::string_view kDefaultShmName = "/mk_exchange_events";

// Default capacity for all exchange IPC queues.
inline constexpr std::uint32_t kExchangeQueueCapacity = 4096;

// Maximum number of Gateway processes (compile-time).
// Each gateway serves exactly one client. The Engine round-robin polls
// all active gateways. 8 is generous for a simulator — a production
// exchange would use a runtime config.
inline constexpr std::uint32_t kMaxGateways = 8;

// =============================================================================
// Queue type aliases
// =============================================================================

static_assert(std::is_trivially_copyable_v<ExchangeEvent>,
              "ExchangeEvent must be trivially copyable for shared memory");

/// Event queue: Engine → Gateway (responses) and Engine → MD Publisher.
using SharedEventQueue =
    sys::memory::FixedSPSCQueue<ExchangeEvent, kExchangeQueueCapacity>;

// =============================================================================
// OrderRequest — Gateway → Engine IPC message
// =============================================================================

/// Request type tag for OrderRequest.
enum class OrderRequestType : std::uint8_t {
  kNewOrder,
  kCancelOrder,
  kModifyOrder,
  kClientConnect,    ///< New TCP client connected (assigns session_id).
  kClientDisconnect, ///< TCP client disconnected (cancels resting orders).
};

/// A single order request from the Gateway to the Engine.
/// Fixed-size POD — same design philosophy as ExchangeEvent.
///
/// Field usage by request type:
///   kNewOrder:          gateway_id, session_id, client_order_id, symbol_id,
///                       side, price, qty, send_ts
///   kCancelOrder:       gateway_id, session_id, client_order_id, symbol_id,
///                       send_ts
///   kModifyOrder:       gateway_id, session_id, client_order_id, symbol_id,
///                       new_price, new_qty, send_ts
///   kClientConnect:     gateway_id, client_order_id (contains accepted fd)
///   kClientDisconnect:  gateway_id, session_id
struct OrderRequest {
  OrderRequestType type{};
  std::uint8_t gateway_id{0}; ///< Which gateway sent this (0-based index).
  std::uint16_t session_id{0};
  std::uint32_t request_seq{0}; ///< Monotonic sequence — copied to responses.
  std::uint64_t client_order_id{0};
  std::uint32_t symbol_id{0};
  algo::Side side{};
  algo::Price price{0};
  algo::Qty qty{0};
  algo::Price new_price{0};
  algo::Qty new_qty{0};
  std::int64_t send_ts{0};
};

static_assert(std::is_trivially_copyable_v<OrderRequest>,
              "OrderRequest must be trivially copyable for shared memory");

/// Request queue: Gateway → Engine (parsed orders).
using SharedRequestQueue =
    sys::memory::FixedSPSCQueue<OrderRequest, kExchangeQueueCapacity>;

// =============================================================================
// SharedExchangeRegion — top-level shared memory layout
// =============================================================================

/// Top-level shared memory layout for multi-gateway exchange.
/// Placed at the start of the POSIX shared memory object via placement new.
///
/// Per-gateway SPSC queue pairs:
///   - request_queues[i]:  Gateway i → Engine (parsed orders)
///   - response_queues[i]: Engine → Gateway i (ack/fill/reject/session events)
/// Shared queue:
///   - md_event_queue: Engine → MD Publisher (fill/BBO events)
///
/// Each gateway serves exactly one client. The Engine round-robin polls
/// all active request queues (up to --num_gateways). Responses are routed
/// to the correct gateway via session_to_gateway[event.session_id]
/// (engine-local table, built from kClientConnect requests).
///
/// Lifecycle:
///   1. exchange_engine: open_shared(kCreateOrOpen) → placement new
///      → engine_ready.store(1, release)
///   2. exchange_gateway[i] + exchange_md_publisher: open_shared(kOpenExisting)
///      → engine_ready.load(acquire) spin-wait
///   3. All processes run their main loops
///   4. Shutdown: any process sets shutdown = 1
///   5. exchange_engine: shm_unlink()
struct SharedExchangeRegion {
  /// Set to 1 by exchange_engine after placement-new initialization.
  alignas(sys::kCacheLineSize) std::atomic<std::uint32_t> engine_ready{0};

  /// Any process sets to 1 on SIGINT/SIGTERM to signal graceful shutdown.
  alignas(sys::kCacheLineSize) std::atomic<std::uint32_t> shutdown{0};

  /// Number of active gateways (set by Engine at startup).
  /// Gateways verify their gateway_id < num_gateways before proceeding.
  alignas(sys::kCacheLineSize) std::atomic<std::uint32_t> num_gateways{0};

  /// Number of registered symbols (set by Engine at startup).
  /// MD publisher reads this instead of its own --symbol_count flag
  /// to prevent silent event drops from mismatch.
  alignas(sys::kCacheLineSize) std::atomic<std::uint32_t> symbol_count{0};

  // NOLINTBEGIN(*-avoid-c-arrays) — C-arrays required for shared memory
  // layout (fixed-size, no heap, trivially copyable).

  /// Per-slot claimed flag. Gateway atomically sets to 1 on startup.
  /// If already 1, another gateway is using this slot → abort.
  /// Prevents duplicate gateway_id from breaking SPSC contract.
  alignas(sys::kCacheLineSize)
      std::atomic<std::uint32_t> gateway_claimed[kMaxGateways]{};

  /// Per-gateway request queues. Gateway[i] pushes to request_queues[i].
  /// Engine round-robin polls all active queues.
  SharedRequestQueue request_queues[kMaxGateways];

  /// Per-gateway response queues. Engine routes events by session_to_gateway.
  /// Gateway[i] pops from response_queues[i].
  SharedEventQueue response_queues[kMaxGateways];

  // NOLINTEND(*-avoid-c-arrays)

  /// Engine → MD Publisher: fill and BBO update events (single publisher).
  SharedEventQueue md_event_queue;
};

// Compile-time guarantees.
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "std::atomic<uint32_t> must be lock-free for cross-process use");

} // namespace mk::app
