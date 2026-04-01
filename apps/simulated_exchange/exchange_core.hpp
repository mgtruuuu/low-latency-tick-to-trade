/**
 * @file exchange_core.hpp
 * @brief Central exchange coordinator — owns matching engines, session
 *        management, and event emission.
 *
 * ExchangeCore is the heart of the simulated exchange. It owns the
 * per-symbol MatchingEngines and emits events into a fixed buffer.
 * Consumers (Gateway, Publisher) read from the buffer independently
 * via shared memory queues — ExchangeCore does not know who is
 * consuming its events.
 *
 * Event bus architecture:
 *   ExchangeCore.submit_order()
 *     → MatchingEngine.submit_order()
 *     → emit(kOrderAccepted), emit(kFill + kBBOUpdate) × N
 *   Engine process drains events via drain_events() after each request
 *   and dispatches to response_queue (Gateway) and md_queue (Publisher).
 *
 * Session management:
 *   FIX ClOrdID (tag 11) is unique per trading day. We enforce per-session
 *   lifetime uniqueness (cleared on disconnect, stricter than FIX). The core
 *   uses a composite key (session_id << 48 | client_order_id) to make
 *   IDs globally unique.
 *
 * Design:
 *   - Zero allocation on hot path (pre-allocated event buffer + FixedHashMap).
 *   - MatchingEngine per symbol (up to MaxSymbols).
 *   - No dependency on MarketDataPublisher or OrderGateway.
 */

#pragma once

#include "exchange_event.hpp"
#include "shared/protocol.hpp"

#include "algo/matching_engine.hpp"
#include "ds/fixed_hash_map.hpp"
// MmapRegion + allocate_hot_rw_region

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

namespace mk::app {

// =============================================================================
// ExchangeCore
// =============================================================================

template <std::size_t MaxSymbols = 2> class ExchangeCore {
  // ---------------------------------------------------------------------------
  // Internal types
  // ---------------------------------------------------------------------------

  struct SymbolSlot {
    algo::MatchingEngine<> engine; ///< Non-owning, caller owns the buffer.
    bool active = false;
  };

  struct IdEntry {
    std::uint64_t exchange_id;
    algo::Side side;
    std::uint32_t symbol_id;
  };

  struct ReverseIdEntry {
    std::uint16_t session_id;
    std::uint64_t client_order_id;
    std::uint32_t symbol_id;
    std::int64_t send_ts; ///< Original order's send_ts (for maker fill RTT).
  };

  // ---------------------------------------------------------------------------
  // Data members
  // ---------------------------------------------------------------------------

  // Per-symbol matching engines.
  std::array<SymbolSlot, MaxSymbols> symbols_{};
  std::uint64_t next_exchange_id_{1};

  // Event buffer — fixed-size, overwritten each drain cycle.
  // Worst case per crossing order:
  //   1 ack + MaxFills × (1 taker fill + 1 maker fill + 2 BBO) = 1 + 64×4 =
  //   257. 512 provides comfortable headroom.
  static constexpr std::size_t kMaxEvents = 512;
  std::array<ExchangeEvent, kMaxEvents> events_{};
  std::uint32_t event_count_{0};

  // Session management.
  std::uint16_t next_session_id_{1};
  std::array<bool, 65536> active_sessions_{}; ///< Wrap-safe: skip active IDs.

  // Session-lifetime duplicate detection.
  static constexpr std::size_t kMaxSeenOrders = 16384;
  ds::FixedHashMap<std::uint64_t, bool, kMaxSeenOrders> seen_orders_;

  // Client ↔ exchange ID mappings.
  static constexpr std::size_t kMaxIdMappings = 1024;
  ds::FixedHashMap<std::uint64_t, IdEntry, kMaxIdMappings> id_map_;
  ds::FixedHashMap<std::uint64_t, ReverseIdEntry, kMaxIdMappings>
      reverse_id_map_;

public:
  /// Maximum events per drain cycle (public for test access).
  static constexpr std::size_t kMaxEventsCapacity = kMaxEvents;

  /// Register a symbol slot. Call once per symbol at startup.
  /// Caller allocates the buffer; ExchangeCore never allocates memory.
  /// Aborts on invalid symbol_id or MatchingEngine creation failure.
  /// @param symbol_id 1-based symbol identifier (must be in [1, MaxSymbols]).
  /// @param buf       Caller-owned buffer for OrderBook backing storage.
  /// @param buf_bytes Size of the buffer in bytes.
  /// @param params    OrderBook capacity params (default: OrderBook defaults).
  void register_symbol(std::uint32_t symbol_id, void *buf,
                       std::size_t buf_bytes,
                       const algo::OrderBook::Params &params =
                           algo::OrderBook::Params{}) noexcept {
    if (symbol_id == 0 || symbol_id > MaxSymbols) {
      std::abort();
    }
    auto &slot = symbols_[symbol_id - 1];

    auto engine = algo::MatchingEngine<>::create(buf, buf_bytes, params);
    if (!engine) {
      std::abort();
    }
    slot.engine = std::move(*engine);
    slot.active = true;
  }

  // ---------------------------------------------------------------------------
  // Session lifecycle (cold path)
  // ---------------------------------------------------------------------------

  /// Assign a session ID for a newly connected client.
  /// Skips IDs that are still active to prevent wrap-around collisions.
  /// Aborts if all 65535 session IDs are exhausted (unreachable in practice
  /// with <10 concurrent clients).
  [[nodiscard]] std::uint16_t on_client_connect() noexcept {
    // Find an unused session ID, skipping 0 (reserved) and active IDs.
    std::uint32_t attempts = 0;
    while (true) {
      if (next_session_id_ == 0) {
        next_session_id_ = 1;
      }
      const auto id = next_session_id_++;
      if (!active_sessions_[id]) {
        active_sessions_[id] = true;
        return id;
      }
      if (++attempts >= 65535) [[unlikely]] {
        std::abort(); // All session IDs exhausted.
      }
    }
  }

  /// Handle one client disconnect — cancel only that session's resting orders.
  void on_client_disconnect(std::uint16_t session_id) noexcept {
    if (session_id == 0) {
      return;
    }

    active_sessions_[session_id] = false;

    // Kill switch scoped to this session only.
    // NOTE: keys_to_erase is 8 KiB with current kMaxIdMappings=1024.
    // This is a cold-path (disconnect-only) stack tradeoff to avoid heap alloc.
    std::array<std::uint64_t, kMaxIdMappings> keys_to_erase{};
    std::size_t erase_count = 0;

    id_map_.for_each([this, session_id, &keys_to_erase, &erase_count](
                         const std::uint64_t &key, IdEntry &entry) noexcept {
      if (extract_session_id(key) != session_id) {
        return;
      }
      (void)symbols_[entry.symbol_id - 1].engine.cancel_order(
          entry.exchange_id);
      assert(erase_count < keys_to_erase.size() &&
             "id_map_ overflow — keys_to_erase must be >= id_map_ capacity");
      keys_to_erase[erase_count++] = key;
    });

    for (std::size_t i = 0; i < erase_count; ++i) {
      // Forward/reverse maps are maintained as a pair — if one has the
      // entry, the other must too. Assert rather than silently skip.
      const auto *fwd = id_map_.find(keys_to_erase[i]);
      assert(fwd && "disconnect cleanup lost forward mapping");

      [[maybe_unused]] const bool erased_rev =
          reverse_id_map_.erase(fwd->exchange_id);
      assert(erased_rev && "disconnect cleanup lost reverse mapping");

      [[maybe_unused]] const bool erased_fwd = id_map_.erase(keys_to_erase[i]);
      assert(erased_fwd &&
             "disconnect cleanup failed to erase forward mapping");
    }

    // Clean up seen_orders_ for this session — frees capacity for future
    // sessions and prevents session_id wrap from causing false duplicates.
    std::array<std::uint64_t, kMaxSeenOrders> seen_to_erase{};
    std::size_t seen_erase_count = 0;
    seen_orders_.for_each([session_id, &seen_to_erase,
                           &seen_erase_count](const std::uint64_t &seen_key,
                                              bool & /*val*/) noexcept {
      if (extract_session_id(seen_key) == session_id) {
        assert(seen_erase_count < seen_to_erase.size() &&
               "seen_orders_ overflow — seen_to_erase must be >= capacity");
        seen_to_erase[seen_erase_count++] = seen_key;
      }
    });
    for (std::size_t i = 0; i < seen_erase_count; ++i) {
      (void)seen_orders_.erase(seen_to_erase[i]);
    }
  }

  // ---------------------------------------------------------------------------
  // Order operations (hot path) — emit events, return void
  // ---------------------------------------------------------------------------

  /// Submit a new order. Emits kOrderAccepted + kFill events, or
  /// kOrderRejected on failure.
  void submit_order(std::uint16_t session_id, const NewOrder &order) noexcept {
    // Reject client_order_id that exceeds composite key range.
    if (order.client_order_id > kMaxClientOrderId) [[unlikely]] {
      emit({.type = EventType::kOrderRejected,
            .session_id = session_id,
            .client_order_id = order.client_order_id,
            .send_ts = order.send_ts,
            .reason = RejectReason::kInvalidOrderId});
      return;
    }

    // Validate symbol.
    std::size_t sym_idx = 0;
    if (!symbol_index(order.symbol_id, sym_idx)) [[unlikely]] {
      emit({.type = EventType::kOrderRejected,
            .session_id = session_id,
            .client_order_id = order.client_order_id,
            .symbol_id = order.symbol_id,
            .send_ts = order.send_ts,
            .reason = RejectReason::kUnknownSymbol});
      return;
    }
    auto &slot = symbols_[sym_idx];

    // Reject duplicate client_order_id within the session lifetime.
    // FIX requires ClOrdID unique per trading day; we enforce per-session
    // (seen_orders_ is cleared on disconnect). Even after cancel/fill,
    // the ID cannot be reused within the same session.
    const auto key = make_composite_key(session_id, order.client_order_id);
    if (seen_orders_.find(key) != nullptr) [[unlikely]] {
      emit({.type = EventType::kOrderRejected,
            .session_id = session_id,
            .client_order_id = order.client_order_id,
            .symbol_id = order.symbol_id,
            .send_ts = order.send_ts,
            .reason = RejectReason::kDuplicateOrderId});
      return;
    }
    // Record this ID as seen (insert-only within session lifetime).
    // Separate from the duplicate check above — insert failure here
    // means capacity full, not duplicate.
    if (!seen_orders_.insert(key, true)) [[unlikely]] {
      emit({.type = EventType::kOrderRejected,
            .session_id = session_id,
            .client_order_id = order.client_order_id,
            .symbol_id = order.symbol_id,
            .send_ts = order.send_ts,
            .reason = RejectReason::kBookFull});
      return;
    }

    // Submit to matching engine.
    const auto exchange_id = next_exchange_id_++;
    const auto result = slot.engine.submit_order(exchange_id, order.side,
                                                 order.price, order.qty);

    const bool accepted = result.rested_in_book() || result.has_fills();

    if (!accepted) {
      emit({.type = EventType::kOrderRejected,
            .session_id = session_id,
            .client_order_id = order.client_order_id,
            .symbol_id = order.symbol_id,
            .send_ts = order.send_ts,
            .reason = RejectReason::kBookFull});
      return;
    }

    // Order accepted.
    emit({.type = EventType::kOrderAccepted,
          .session_id = session_id,
          .client_order_id = order.client_order_id,
          .exchange_order_id = exchange_id,
          .symbol_id = order.symbol_id,
          .send_ts = order.send_ts});

    // Emit fill events with BBO snapshot after each fill.
    // BBO is captured immediately while the book state is still fresh,
    // before subsequent orders in the same poll_once() batch modify it.
    algo::Qty remaining = order.qty;
    for (const auto &fill : result.fills) {
      remaining -= fill.qty;

      // Taker fill (the client who sent this order).
      emit({.type = EventType::kFill,
            .session_id = session_id,
            .client_order_id = order.client_order_id,
            .exchange_order_id = exchange_id,
            .symbol_id = order.symbol_id,
            .side = order.side,
            .price = fill.price,
            .qty = fill.qty,
            .remaining_qty = remaining,
            .send_ts = order.send_ts});

      // Maker fill (the resting order owner — possibly on a different gateway).
      emit_maker_fill(slot, fill, order.side);

      emit_bbo(slot, order.symbol_id);
    }

    // Register ID mapping for resting orders (cancel/modify + maker fill).
    // key was already computed above for duplicate check.
    if (result.rested_in_book()) {
      (void)id_map_.insert(key,
                           IdEntry{exchange_id, order.side, order.symbol_id});
      (void)reverse_id_map_.insert(
          exchange_id, ReverseIdEntry{session_id, order.client_order_id,
                                      order.symbol_id, order.send_ts});
    }
  }

  /// Cancel a resting order. Emits kCancelAck or kCancelRejected.
  void cancel_order(std::uint16_t session_id,
                    const CancelOrder &cancel) noexcept {
    // Reject client_order_id that exceeds composite key range.
    if (cancel.client_order_id > kMaxClientOrderId) [[unlikely]] {
      emit({.type = EventType::kCancelRejected,
            .session_id = session_id,
            .client_order_id = cancel.client_order_id,
            .send_ts = cancel.send_ts,
            .reason = RejectReason::kInvalidOrderId});
      return;
    }

    // Validate symbol.
    std::size_t sym_idx = 0;
    if (!symbol_index(cancel.symbol_id, sym_idx)) [[unlikely]] {
      emit({.type = EventType::kCancelRejected,
            .session_id = session_id,
            .client_order_id = cancel.client_order_id,
            .send_ts = cancel.send_ts,
            .reason = RejectReason::kUnknownSymbol});
      return;
    }
    auto &slot = symbols_[sym_idx];

    // Look up client_order_id -> exchange_order_id mapping.
    const auto key = make_composite_key(session_id, cancel.client_order_id);
    const auto *entry = id_map_.find(key);
    if (!entry) {
      emit({.type = EventType::kCancelRejected,
            .session_id = session_id,
            .client_order_id = cancel.client_order_id,
            .send_ts = cancel.send_ts,
            .reason = RejectReason::kOrderNotFound});
      return;
    }

    // Cancel the order in the matching engine.
    const bool cancelled = slot.engine.cancel_order(entry->exchange_id);
    if (!cancelled) {
      emit({.type = EventType::kCancelRejected,
            .session_id = session_id,
            .client_order_id = cancel.client_order_id,
            .send_ts = cancel.send_ts,
            .reason = RejectReason::kOrderNotFound});
      return;
    }

    // Cancel succeeded — remove from both ID mappings.
    [[maybe_unused]] const bool erased_rev =
        reverse_id_map_.erase(entry->exchange_id);
    assert(erased_rev && "cancel: forward entry exists but reverse missing");
    [[maybe_unused]] const bool erased_fwd = id_map_.erase(key);
    assert(erased_fwd && "cancel: forward erase failed after successful find");
    emit({.type = EventType::kCancelAck,
          .session_id = session_id,
          .client_order_id = cancel.client_order_id,
          .send_ts = cancel.send_ts});
  }

  /// Modify a resting order (cancel + re-submit). Emits kModifyAck +
  /// optional kFill events, or kModifyRejected on failure.
  void modify_order(std::uint16_t session_id,
                    const ModifyOrder &modify) noexcept {
    // Reject client_order_id that exceeds composite key range.
    if (modify.client_order_id > kMaxClientOrderId) [[unlikely]] {
      emit({.type = EventType::kModifyRejected,
            .session_id = session_id,
            .client_order_id = modify.client_order_id,
            .send_ts = modify.send_ts,
            .reason = RejectReason::kInvalidOrderId});
      return;
    }

    // Validate symbol.
    std::size_t sym_idx = 0;
    if (!symbol_index(modify.symbol_id, sym_idx)) [[unlikely]] {
      emit({.type = EventType::kModifyRejected,
            .session_id = session_id,
            .client_order_id = modify.client_order_id,
            .send_ts = modify.send_ts,
            .reason = RejectReason::kUnknownSymbol});
      return;
    }
    auto &slot = symbols_[sym_idx];

    // Look up client_order_id -> IdEntry mapping.
    const auto modify_key =
        make_composite_key(session_id, modify.client_order_id);
    const auto *entry = id_map_.find(modify_key);
    if (!entry) {
      emit({.type = EventType::kModifyRejected,
            .session_id = session_id,
            .client_order_id = modify.client_order_id,
            .send_ts = modify.send_ts,
            .reason = RejectReason::kOrderNotFound});
      return;
    }

    // Assign new exchange ID for the modified order.
    const auto new_exchange_id = next_exchange_id_++;

    // Cancel old + re-submit with new price/qty.
    const auto result = slot.engine.modify_order(
        entry->exchange_id, entry->side, modify.new_price, modify.new_qty,
        new_exchange_id);

    if (!result.success) {
      emit({.type = EventType::kModifyRejected,
            .session_id = session_id,
            .client_order_id = modify.client_order_id,
            .send_ts = modify.send_ts,
            .reason = RejectReason::kOrderNotFound});
      return;
    }

    // Save entry fields before erasing — accessing a logically-erased
    // slot (tombstone) is undefined behaviour even though it works in
    // practice (tombstone deletion preserves memory contents).
    const auto saved_side = entry->side;
    const auto saved_symbol_id = entry->symbol_id;

    // Update ID mappings: remove old, insert new (if resting).
    [[maybe_unused]] const bool mod_rev =
        reverse_id_map_.erase(entry->exchange_id);
    assert(mod_rev && "modify: forward entry exists but reverse missing");
    [[maybe_unused]] const bool mod_fwd = id_map_.erase(modify_key);
    assert(mod_fwd && "modify: forward erase failed after successful find");
    if (result.rested_in_book()) {
      (void)id_map_.insert(
          modify_key, IdEntry{new_exchange_id, saved_side, saved_symbol_id});
      (void)reverse_id_map_.insert(
          new_exchange_id, ReverseIdEntry{session_id, modify.client_order_id,
                                          saved_symbol_id, modify.send_ts});
    }

    // Emit ModifyAck.
    emit({.type = EventType::kModifyAck,
          .session_id = session_id,
          .client_order_id = modify.client_order_id,
          .exchange_order_id = new_exchange_id,
          .symbol_id = modify.symbol_id,
          .send_ts = modify.send_ts});

    // Emit fill events with BBO snapshot after each fill.
    algo::Qty remaining = modify.new_qty;
    for (const auto &fill : result.fills) {
      remaining -= fill.qty;

      // Taker fill.
      emit({.type = EventType::kFill,
            .session_id = session_id,
            .client_order_id = modify.client_order_id,
            .exchange_order_id = new_exchange_id,
            .symbol_id = modify.symbol_id,
            .side = saved_side,
            .price = fill.price,
            .qty = fill.qty,
            .remaining_qty = remaining,
            .send_ts = modify.send_ts});

      // Maker fill (resting order owner).
      emit_maker_fill(slot, fill, saved_side);

      emit_bbo(slot, modify.symbol_id);
    }
  }

  // ---------------------------------------------------------------------------
  // Event buffer access
  // ---------------------------------------------------------------------------

  /// Current number of events in the buffer (before drain).
  [[nodiscard]] std::uint32_t event_count() const noexcept {
    return event_count_;
  }

  /// Drain all pending events. Returns a span valid until the next
  /// submit/cancel/modify call. Resets the event count.
  [[nodiscard]] std::span<const ExchangeEvent> drain_events() noexcept {
    auto result = std::span<const ExchangeEvent>(events_.data(), event_count_);
    event_count_ = 0;
    return result;
  }

  // ---------------------------------------------------------------------------
  // Accessors
  // ---------------------------------------------------------------------------

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
  // ---------------------------------------------------------------------------
  // Private functions
  // ---------------------------------------------------------------------------

  void emit(ExchangeEvent event) noexcept {
    if (event_count_ >= kMaxEvents) [[unlikely]] {
      // Event buffer overflow is unrecoverable — it means TCP responses
      // and/or market data events will be silently lost. Abort rather
      // than corrupt downstream state. Increase kMaxEvents if this fires.
      std::abort();
    }
    events_[event_count_++] = event;
  }

  /// Maximum client_order_id that fits in the 48-bit composite key.
  /// IDs above this are rejected to prevent aliasing (upper bits silently
  /// dropped by the mask would cause different orders to share the same key).
  static constexpr std::uint64_t kMaxClientOrderId = 0x0000'FFFF'FFFF'FFFF;

  [[nodiscard]] static std::uint64_t
  make_composite_key(std::uint16_t session_id,
                     std::uint64_t client_order_id) noexcept {
    assert(client_order_id <= kMaxClientOrderId);
    return (static_cast<std::uint64_t>(session_id) << 48) |
           (client_order_id & kMaxClientOrderId);
  }

  [[nodiscard]] static std::uint16_t
  extract_session_id(std::uint64_t composite_key) noexcept {
    return static_cast<std::uint16_t>((composite_key >> 48) & 0xFFFF);
  }

  [[nodiscard]] bool symbol_index(std::uint32_t symbol_id,
                                  std::size_t &idx) const noexcept {
    if (symbol_id == 0 || symbol_id > MaxSymbols) {
      return false;
    }
    idx = symbol_id - 1;
    return symbols_[idx].active;
  }

  /// Emit a fill event for the maker (resting order owner).
  /// Emit a fill event for the maker (resting) side of a trade.
  ///
  /// Why this is needed: the taker's fill is easy — we know their session
  /// because they just sent the order. But the maker's order has been
  /// resting in the book, possibly from a different session on a different
  /// gateway. We must look up reverse_id_map_ to find the maker's
  /// session_id and client_order_id, then route the fill to the correct
  /// gateway via session_to_gateway[] (cross-gateway fill routing).
  ///
  /// If the maker order is fully consumed, clean up both id maps.
  void emit_maker_fill(const SymbolSlot &slot, const algo::Fill &fill,
                       algo::Side taker_side) noexcept {
    const auto *maker = reverse_id_map_.find(fill.maker_id);
    if (!maker) {
      return; // maker not in our map (shouldn't happen)
    }

    // Maker side is opposite of taker side.
    const auto maker_side =
        (taker_side == algo::Side::kBid) ? algo::Side::kAsk : algo::Side::kBid;

    // Query book for maker's remaining qty after this fill.
    // If order still exists → partial fill. If removed → full fill (qty=0).
    const auto maker_remaining = slot.engine.book().order_qty(fill.maker_id);

    emit({.type = EventType::kFill,
          .session_id = maker->session_id,
          .client_order_id = maker->client_order_id,
          .exchange_order_id = fill.maker_id,
          .symbol_id = maker->symbol_id,
          .side = maker_side,
          .price = fill.price,
          .qty = fill.qty,
          .remaining_qty = maker_remaining,
          .send_ts = maker->send_ts}); // echo maker's original send_ts

    // If maker order fully filled, clean up both maps.
    if (!slot.engine.book().has_order(fill.maker_id)) {
      [[maybe_unused]] const bool fill_fwd = id_map_.erase(
          make_composite_key(maker->session_id, maker->client_order_id));
      assert(fill_fwd && "maker fill: forward mapping missing");
      [[maybe_unused]] const bool fill_rev =
          reverse_id_map_.erase(fill.maker_id);
      assert(fill_rev && "maker fill: reverse mapping missing");
    }
  }

  /// Emit BBO update events for a symbol. Called immediately after each
  /// fill while the book state is still fresh — captures the exact post-fill
  /// BBO before subsequent orders in the same poll_once() batch can modify it.
  void emit_bbo(const SymbolSlot &slot, std::uint32_t symbol_id) noexcept {
    const auto &book = slot.engine.book();
    const auto &bids = book.bids();
    const auto &asks = book.asks();

    if (!bids.empty()) {
      const auto &best = bids.front();
      emit({.type = EventType::kBBOUpdate,
            .symbol_id = symbol_id,
            .side = algo::Side::kBid,
            .price = best.price,
            .qty = best.total_qty});
    }
    if (!asks.empty()) {
      const auto &best = asks.front();
      emit({.type = EventType::kBBOUpdate,
            .symbol_id = symbol_id,
            .side = algo::Side::kAsk,
            .price = best.price,
            .qty = best.total_qty});
    }
  }
};

} // namespace mk::app
