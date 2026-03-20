/**
 * @file order_manager.hpp
 * @brief Order lifecycle management with pre-trade risk and kill switch.
 *
 * Converts strategy signals into NewOrder messages, assigns client order
 * IDs, and tracks outstanding orders and per-symbol net position.
 *
 * Pre-trade risk checks (checked on every order, cheapest first):
 *   1. Kill switch — blocks ALL new orders when active
 *   2. Max outstanding orders — limits concurrent in-flight orders
 *   3. Max order size — rejects oversized orders (exchange-imposed bands)
 *   4. Max notional — limits price * qty per order (fat-finger protection)
 *   5. Order rate limit — token bucket (exchange msg/sec limits)
 *   6. Per-symbol position limit — worst-case fill projection
 *
 * Kill switch:
 *   Emergency halt mechanism. When triggered (via SIGUSR1 or automatic
 *   consecutive-reject detection), the OMS:
 *     1. Blocks all new orders and modifies immediately
 *     2. Collects CancelOrder messages for all outstanding orders
 *     3. Caller sends the cancels, then drains CancelAck/CancelReject
 *     4. Once all cancel responses arrive, state transitions to kComplete
 *
 *   State machine: kNormal → kCancelling → kDraining → kComplete
 *   In real HFT: kill switch is mandated by exchanges (e.g., CME iLink
 *   Mass Quote Cancel, FIX OrderMassCancelRequest MsgType=q).
 *
 * Modify-or-new logic:
 *   When a signal arrives for a (symbol, side) that already has a resting
 *   order, the OMS sends a ModifyOrder instead of a new order. This avoids
 *   cancel + new-order round trips and is standard market-making behavior:
 *   re-price your resting quote as the BBO moves.
 *
 * Multi-symbol:
 *   Position is tracked per symbol. FillReport does not carry symbol_id,
 *   so the OrderInfo stored at order submission time records which symbol
 *   the order belongs to. Fill processing uses this stored symbol_id.
 *
 * Design:
 *   - Zero allocation on hot path. All scaling state (positions, active
 *     orders, outstanding map, cancel buffer) lives in a caller-provided
 *     StrategyCtx pre-allocated from a contiguous MmapRegion.
 *   - Returns bool + output ref (hot-path pattern, avoids optional overhead).
 */

#pragma once

#include "strategy_ctx.hpp"
#include "strategy_policy.hpp"

#include "sys/log/signal_logger.hpp"
#include "sys/nano_clock.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <span>

namespace mk::app {

class OrderManager {
public:
  /// Kill switch state machine.
  /// kNormal → kCancelling (transient) → kDraining → kComplete
  ///
  /// kNormal:     Normal operation. All risk checks apply.
  /// kCancelling: Kill switch triggered. Collecting cancel orders.
  ///              Transitions to kDraining within trigger_kill_switch().
  /// kDraining:   All cancels collected. Waiting for CancelAck/CancelReject.
  /// kComplete:   All cancel responses received. System halted.
  enum class KillSwitchState : std::uint8_t {
    kNormal = 0,
    kCancelling,
    kDraining,
    kComplete,
  };

  /// Batch of CancelOrder messages produced by trigger_kill_switch().
  /// Span views into the cancel buffer pre-allocated in StrategyCtx.
  struct CancelBatch {
    std::span<CancelOrder> cancels;
    std::uint32_t count{0};
  };

  /// Constructs an OrderManager operating on caller-provided StrategyCtx.
  /// StrategyCtx must outlive this OrderManager.
  ///
  /// @param ctx              Pre-allocated buffer context (from make_order_ctx).
  /// @param max_position     Maximum absolute net position per symbol.
  /// @param max_order_size   Maximum quantity per single order.
  /// @param max_notional     Maximum notional (price * qty) per order.
  /// @param max_orders_per_window Token bucket capacity (= max burst size).
  /// @param rate_window_ns   Token refill period in nanoseconds (default: 1s).
  /// @param order_timeout_ns Order ack timeout in nanoseconds. Orders without
  ///                         ack/fill within this window are auto-cancelled.
  explicit OrderManager(
      StrategyCtx &ctx,
      std::int64_t max_position = 100,
      algo::Qty max_order_size = 1000,
      std::int64_t max_notional = 50'000'000,
      std::uint32_t max_orders_per_window = 100,
      std::int64_t rate_window_ns = 1'000'000'000,
      std::int64_t order_timeout_ns = 5'000'000'000) noexcept
      : ctx_(ctx),
        max_position_(max_position),
        max_order_size_(max_order_size),
        max_notional_(max_notional),
        bucket_capacity_(max_orders_per_window),
        refill_interval_ns_(max_orders_per_window > 0
                                ? rate_window_ns / max_orders_per_window
                                : rate_window_ns),
        tokens_(max_orders_per_window),
        outstanding_(ctx.map_buf, ctx.map_buf_size, ctx.map_capacity),
        timeout_wheel_(ctx.timeout_wheel_buf, ctx.timeout_wheel_buf_size,
                       ctx.timeout_wheel_size, ctx.timeout_max_timers),
        order_timeout_ns_(order_timeout_ns),
        timeout_ids_(ctx.timeout_ids) {
    active_instance = this;
  }

  ~OrderManager() = default;

  // Non-copyable, non-movable (holds reference to StrategyCtx).
  OrderManager(const OrderManager &) = delete;
  OrderManager &operator=(const OrderManager &) = delete;
  OrderManager(OrderManager &&) = delete;
  OrderManager &operator=(OrderManager &&) = delete;

  /// Check if the signal should modify an existing resting order instead
  /// of placing a new one. Call this BEFORE on_signal().
  ///
  /// If there's already a resting order on the same (symbol, side), writes
  /// a ModifyOrder into @p out. The caller should send it and skip
  /// on_signal(). If false, proceed with on_signal() as usual.
  /// @param signal Strategy signal (symbol_id pre-validated by caller).
  /// @param out    ModifyOrder written only when returning true.
  [[nodiscard]] bool check_modify(const Signal &signal,
                                  ModifyOrder &out) noexcept {
    // Kill switch blocks modifies — no point re-pricing if we're halting.
    if (kill_switch_state_ != KillSwitchState::kNormal) [[unlikely]] {
      return false;
    }

    // symbol_id validated at event loop boundary — direct index.
    const auto sym_idx = signal.symbol_id - 1;
    const auto side_idx = static_cast<std::size_t>(signal.side);
    auto &active =
        ctx_.active_orders[(sym_idx * StrategyCtx::kSideCount) + side_idx];

    if (!active.resting) {
      return false; // No resting order to modify
    }

    // Same price → no point modifying.
    if (active.price == signal.price) {
      return false;
    }

    out.client_order_id = active.client_order_id;
    out.symbol_id = signal.symbol_id;
    out.new_price = signal.price;
    out.new_qty = signal.qty;
    out.send_ts = sys::monotonic_nanos();

    // Update tracked price (optimistic — will revert on reject).
    active.price = signal.price;
    active.qty = signal.qty;
    modifies_sent_.fetch_add(1, std::memory_order_relaxed);

    return true;
  }

  /// Generate a NewOrder from a strategy signal.
  /// @param signal Strategy signal (symbol_id pre-validated by caller).
  /// @param out    NewOrder written only when returning true.
  /// @return true if order generated, false if risk limits breached.
  [[nodiscard]] bool on_signal(const Signal &signal, NewOrder &out) noexcept {
    // Risk check 0: kill switch blocks ALL new orders.
    if (kill_switch_state_ != KillSwitchState::kNormal) [[unlikely]] {
      risk_rejects_killswitch_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    // Risk check 1: outstanding order limit (cheapest — integer compare).
    if (outstanding_count_ >= ctx_.max_outstanding) [[unlikely]] {
      risk_rejects_outstanding_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    // symbol_id validated at event loop boundary — direct index.
    const auto sym_idx = signal.symbol_id - 1;

    // Risk check 2: per-symbol outstanding limit.
    if (ctx_.outstanding_per_symbol[sym_idx] >=
        ctx_.max_outstanding_per_symbol) [[unlikely]] {
      risk_rejects_per_symbol_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    // Risk check 3: max order size (single integer compare).
    if (signal.qty > max_order_size_) [[unlikely]] {
      risk_rejects_size_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    // Risk check 3: max notional per order (one multiply + compare).
    // Price is int64_t, Qty is uint32_t — product fits in int64_t for
    // typical tick prices (< 2^31) and quantities (< 2^31).
    const auto notional =
        signal.price * static_cast<std::int64_t>(signal.qty);
    if (notional > max_notional_) [[unlikely]] {
      risk_rejects_notional_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    // Risk check 4: order rate limit (token bucket).
    // Real exchanges impose per-second message limits (e.g., CME: 50
    // msgs/sec per session, Nasdaq: configurable). Token bucket refills
    // tokens at a constant rate — no boundary-burst problem unlike
    // fixed-window counters.
    {
      const auto now = sys::monotonic_nanos();
      // Refill tokens based on elapsed time since last refill.
      // Each refill_interval_ns_ adds one token, up to bucket_capacity_.
      if (const auto elapsed = now - last_refill_ts_;
          elapsed >= refill_interval_ns_) {
        const auto new_tokens = static_cast<std::uint32_t>(
            elapsed / refill_interval_ns_);
        tokens_ = std::min(tokens_ + new_tokens, bucket_capacity_);
        // Advance by exact multiple of refill_interval_ns_ — don't lose
        // fractional time. E.g., if elapsed=25ms and interval=10ms,
        // add 2 tokens and advance by 20ms (not 25ms).
        last_refill_ts_ += static_cast<std::int64_t>(new_tokens) *
                           refill_interval_ns_;
      }
      if (tokens_ == 0) [[unlikely]] {
        risk_rejects_rate_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
    }

    // Risk check 5: per-symbol position limit.
    // Simulate the worst-case position if this order fills.
    auto projected_position = ctx_.net_position[sym_idx];
    if (signal.side == algo::Side::kBid) {
      projected_position += signal.qty;
    } else {
      projected_position -= signal.qty;
    }

    // Check absolute position against limit.
    const auto abs_pos =
        projected_position < 0 ? -projected_position : projected_position;
    if (abs_pos > max_position_) [[unlikely]] {
      risk_rejects_position_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    // All risk checks passed — generate order.
    out.client_order_id = next_order_id_++;
    out.symbol_id = signal.symbol_id;
    out.side = signal.side;
    out.price = signal.price;
    out.qty = signal.qty;
    out.send_ts = sys::monotonic_nanos();

    // Track outstanding order (store symbol_id for fill processing,
    // since FillReport does not carry symbol_id).
    OrderInfo info{};
    info.side = signal.side;
    info.price = signal.price;
    info.qty = signal.qty;
    info.symbol_id = signal.symbol_id;
    info.send_ts = out.send_ts;

    // Schedule order timeout timer before inserting into map
    // (so timer_handle is stored with the entry in a single insert).
    const auto delay_ticks = static_cast<ds::TimingWheel::tick_t>(
        order_timeout_ns_ / kTickIntervalNs);
    info.timer_handle = timeout_wheel_.schedule(
        on_order_timeout,
        reinterpret_cast<void *>(
            static_cast<std::uintptr_t>(out.client_order_id)),
        delay_ticks);

    const bool inserted = outstanding_.insert(out.client_order_id, info);
    if (!inserted) [[unlikely]] {
      // Should never happen: capacity = 2 * max_outstanding, and
      // outstanding_count_ < max_outstanding is checked at entry.
      sys::log::signal_log(
          "[RISK] Outstanding map insert failed — capacity reached\n");
      timeout_wheel_.cancel(info.timer_handle);
      return false;
    }
    ++outstanding_count_;
    ++ctx_.outstanding_per_symbol[sym_idx];

    // Track as active resting order for this (symbol, side).
    // Optimistic: assume it will rest. If fully filled or rejected,
    // the ack/fill handler clears it.
    auto &active = ctx_.active_orders[(sym_idx * StrategyCtx::kSideCount) +
                                      static_cast<std::size_t>(signal.side)];
    active.client_order_id = out.client_order_id;
    active.price = signal.price;
    active.qty = signal.qty;
    active.resting = true;

    // Consume one token after successful order generation.
    --tokens_;

    return true;
  }

  // -- Kill switch --

  /// Trigger the kill switch. Populates batch with CancelOrder messages
  /// for all outstanding orders. Caller must serialize and send each one.
  ///
  /// State transition: kNormal → kDraining.
  /// If no outstanding orders, transitions directly to kComplete.
  ///
  /// @return CancelBatch with cancel messages to send (span into ctx).
  [[nodiscard]] CancelBatch trigger_kill_switch() noexcept {
    CancelBatch batch{
        .cancels = {ctx_.cancel_buf, ctx_.max_outstanding},
        .count = 0,
    };

    if (kill_switch_state_ != KillSwitchState::kNormal) {
      return batch; // Already triggered — idempotent.
    }

    kill_switch_state_ = KillSwitchState::kCancelling;
    const auto now = sys::monotonic_nanos();

    outstanding_.for_each(
        [&](const std::uint64_t &order_id, const OrderInfo &info) {
          if (batch.count < ctx_.max_outstanding) {
            auto &cancel = batch.cancels[batch.count];
            cancel.client_order_id = order_id;
            cancel.symbol_id = info.symbol_id;
            cancel.send_ts = now;
            ++batch.count;
          }
        });

    // All pending timeouts are irrelevant — kill switch handles all cancels.
    timeout_wheel_.reset();

    pending_cancel_count_ = batch.count;

    if (pending_cancel_count_ == 0) {
      kill_switch_state_ = KillSwitchState::kComplete;
      sys::log::signal_log("[RISK] Kill switch triggered, no outstanding "
                           "orders — complete\n");
    } else {
      kill_switch_state_ = KillSwitchState::kDraining;
      sys::log::signal_log("[RISK] Kill switch triggered, cancelling ",
                           batch.count, " orders\n");
    }

    return batch;
  }

  // -- Exchange response handlers --

  /// Process an OrderAck from the exchange.
  void on_order_ack(const OrderAck &ack) noexcept {
    // Order was accepted — nothing to do beyond logging.
    // The order remains outstanding until filled or cancelled.
    (void)ack;
    acks_received_.fetch_add(1, std::memory_order_relaxed);
  }

  /// Process an OrderReject from the exchange.
  void on_order_reject(const OrderReject &reject) noexcept {
    // Order was rejected — remove from outstanding and active tracking.
    auto *info = outstanding_.find(reject.client_order_id);
    if (info != nullptr) {
      timeout_wheel_.cancel(info->timer_handle);
      clear_active_if_match(info->symbol_id, info->side,
                            reject.client_order_id);
      --ctx_.outstanding_per_symbol[info->symbol_id - 1];
    }
    outstanding_.erase(reject.client_order_id);
    if (outstanding_count_ > 0) {
      --outstanding_count_;
    }
    maybe_compact_outstanding();
    rejects_received_.fetch_add(1, std::memory_order_relaxed);
  }

  /// Process a FillReport from the exchange.
  void on_fill(const FillReport &fill) noexcept {
    // Update per-symbol position.
    // FillReport does not carry symbol_id, so we look up the stored
    // OrderInfo to determine which symbol's position to update.
    auto *info = outstanding_.find(fill.client_order_id);
    if (info != nullptr) {
      const auto sym_idx = info->symbol_id - 1;
      if (sym_idx < ctx_.max_symbols) {
        if (info->side == algo::Side::kBid) {
          ctx_.net_position[sym_idx] += fill.fill_qty;
        } else {
          ctx_.net_position[sym_idx] -= fill.fill_qty;
        }
      }
    }

    // If fully filled (remaining_qty == 0), remove from outstanding.
    if (fill.remaining_qty == 0) {
      // Clear active order tracking for this (symbol, side).
      if (info != nullptr) {
        timeout_wheel_.cancel(info->timer_handle);
        clear_active_if_match(info->symbol_id, info->side,
                              fill.client_order_id);
        --ctx_.outstanding_per_symbol[info->symbol_id - 1];
      }
      outstanding_.erase(fill.client_order_id);
      if (outstanding_count_ > 0) {
        --outstanding_count_;
      }
      maybe_compact_outstanding();
    }

    fills_received_.fetch_add(1, std::memory_order_relaxed);
    total_fill_qty_.fetch_add(fill.fill_qty, std::memory_order_relaxed);
  }

  /// Process a ModifyAck from the exchange.
  void on_modify_ack(const ModifyAck &ack) noexcept {
    // Modify was accepted — active order price/qty already updated
    // optimistically in check_modify(). Nothing to revert.
    (void)ack;
    modifies_acked_.fetch_add(1, std::memory_order_relaxed);
  }

  /// Process a ModifyReject from the exchange.
  void on_modify_reject(const ModifyReject &reject) noexcept {
    // Modify was rejected — the resting order at the old price may
    // still exist (e.g., race condition) or may have been filled.
    // Clear the active tracking to avoid stale state; the next signal
    // will place a fresh order.
    auto *info = outstanding_.find(reject.client_order_id);
    if (info != nullptr) {
      clear_active_if_match(info->symbol_id, info->side,
                            reject.client_order_id);
    }
    modifies_rejected_.fetch_add(1, std::memory_order_relaxed);
  }

  /// Process a CancelAck from the exchange.
  void on_cancel_ack(const CancelAck &ack) noexcept {
    auto *info = outstanding_.find(ack.client_order_id);
    if (info != nullptr) {
      timeout_wheel_.cancel(info->timer_handle);
      clear_active_if_match(info->symbol_id, info->side,
                            ack.client_order_id);
      --ctx_.outstanding_per_symbol[info->symbol_id - 1];
    }
    outstanding_.erase(ack.client_order_id);
    if (outstanding_count_ > 0) {
      --outstanding_count_;
    }
    maybe_compact_outstanding();

    // Kill switch draining: track cancel responses.
    check_drain_complete();
  }

  /// Process a CancelReject from the exchange.
  /// Cancel may be rejected if the order was already filled or cancelled.
  /// During kill switch draining, still counts toward pending cancels.
  void on_cancel_reject(const CancelReject &reject) noexcept {
    // The order may have been filled between cancel-send and reject-recv.
    // Clean up if it's still tracked.
    auto *info = outstanding_.find(reject.client_order_id);
    if (info != nullptr) {
      timeout_wheel_.cancel(info->timer_handle);
      clear_active_if_match(info->symbol_id, info->side,
                            reject.client_order_id);
      --ctx_.outstanding_per_symbol[info->symbol_id - 1];
      outstanding_.erase(reject.client_order_id);
      if (outstanding_count_ > 0) {
        --outstanding_count_;
      }
      maybe_compact_outstanding();
    }

    // Kill switch draining: track cancel responses.
    check_drain_complete();
  }

  // -- Observers --

  /// Net position for a specific symbol (1-based symbol_id).
  [[nodiscard]] std::int64_t
  net_position(std::uint32_t symbol_id = 1) const noexcept {
    return ctx_.net_position[symbol_id - 1];
  }
  /// Total net position across all symbols.
  [[nodiscard]] std::int64_t total_net_position() const noexcept {
    std::int64_t total = 0;
    for (std::uint32_t i = 0; i < ctx_.max_symbols; ++i) {
      total += ctx_.net_position[i];
    }
    return total;
  }
  [[nodiscard]] std::uint32_t outstanding_count() const noexcept {
    return outstanding_count_;
  }
  [[nodiscard]] std::uint64_t orders_sent() const noexcept {
    return next_order_id_ - 1;
  }
  [[nodiscard]] std::uint64_t fills_received() const noexcept {
    return fills_received_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t acks_received() const noexcept {
    return acks_received_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t rejects_received() const noexcept {
    return rejects_received_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t total_fill_qty() const noexcept {
    return total_fill_qty_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t modifies_sent() const noexcept {
    return modifies_sent_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t modifies_acked() const noexcept {
    return modifies_acked_.load(std::memory_order_relaxed);
  }

  // -- Order timeout --

  /// Advance the timeout wheel and generate CancelOrders for timed-out orders.
  /// Called each event loop iteration with the current monotonic timestamp.
  /// Returns a CancelBatch that the caller must serialize and send to exchange.
  [[nodiscard]] CancelBatch advance_timeouts(std::int64_t now_ns) noexcept {
    CancelBatch batch{
        .cancels = {ctx_.cancel_buf, ctx_.max_outstanding},
        .count = 0,
    };

    // Skip during kill switch — kill switch handles all cancels.
    if (kill_switch_state_ != KillSwitchState::kNormal) {
      return batch;
    }

    // Compute elapsed ticks since last advance.
    const auto elapsed_ns = now_ns - last_advance_ns_;
    if (elapsed_ns < kTickIntervalNs) {
      return batch; // Less than one tick elapsed.
    }

    const auto elapsed_ticks =
        static_cast<std::uint32_t>(elapsed_ns / kTickIntervalNs);

    // Reset batch counter before ticking (callbacks append to timeout_ids_).
    timeout_count_ = 0;

    // Advance the wheel. Callbacks fire during tick().
    for (std::uint32_t t = 0; t < elapsed_ticks; ++t) {
      timeout_wheel_.tick();
    }

    // Advance by exact multiple of tick interval (don't lose fractional time).
    last_advance_ns_ +=
        static_cast<std::int64_t>(elapsed_ticks) * kTickIntervalNs;

    // Process collected timeouts → generate CancelOrders.
    for (std::uint32_t i = 0; i < timeout_count_; ++i) {
      const auto coid = timeout_ids_[i];
      auto *info = outstanding_.find(coid);
      if (info == nullptr) {
        continue; // Already filled/cancelled between callback and here.
      }

      if (batch.count < ctx_.max_outstanding) {
        auto &cancel = batch.cancels[batch.count];
        cancel.client_order_id = coid;
        cancel.symbol_id = info->symbol_id;
        cancel.send_ts = now_ns;
        ++batch.count;
      }

      clear_active_if_match(info->symbol_id, info->side, coid);
      --ctx_.outstanding_per_symbol[info->symbol_id - 1];
      outstanding_.erase(coid);
      if (outstanding_count_ > 0) {
        --outstanding_count_;
      }
      timeouts_fired_.fetch_add(1, std::memory_order_relaxed);
    }
    maybe_compact_outstanding();

    if (batch.count > 0) {
      sys::log::signal_log("[RISK] Order timeout: cancelling ", batch.count,
                           " timed-out orders\n");
    }

    return batch;
  }

  // -- Kill switch observers --

  [[nodiscard]] KillSwitchState kill_switch_state() const noexcept {
    return kill_switch_state_;
  }
  [[nodiscard]] bool is_killed() const noexcept {
    return kill_switch_state_ != KillSwitchState::kNormal;
  }

  // -- Risk rejection counters --

  [[nodiscard]] std::uint64_t risk_rejects_outstanding() const noexcept {
    return risk_rejects_outstanding_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t risk_rejects_per_symbol() const noexcept {
    return risk_rejects_per_symbol_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t risk_rejects_position() const noexcept {
    return risk_rejects_position_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t risk_rejects_size() const noexcept {
    return risk_rejects_size_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t risk_rejects_notional() const noexcept {
    return risk_rejects_notional_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t risk_rejects_rate() const noexcept {
    return risk_rejects_rate_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t risk_rejects_killswitch() const noexcept {
    return risk_rejects_killswitch_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t timeouts_fired() const noexcept {
    return timeouts_fired_.load(std::memory_order_relaxed);
  }

private:
  /// Tick interval for the timeout wheel: 1ms.
  static constexpr std::int64_t kTickIntervalNs = 1'000'000;

  /// Static callback invoked by TimingWheel::tick() when a timer expires.
  /// Single-threaded HFT pattern: one OrderManager per strategy thread,
  /// static pointer avoids indirection in the callback.
  /// ctx carries client_order_id packed as void*.
  static void on_order_timeout(void *ctx) noexcept {
    const auto coid =
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ctx));
    auto *self = active_instance;
    assert(self->timeout_count_ < self->ctx_.max_outstanding);
    self->timeout_ids_[self->timeout_count_++] = coid;
  }
  /// Compact the outstanding map when fully drained and tombstones accumulate.
  /// Tombstones build up with high-churn insert/erase cycles (e.g. replay mode
  /// where orders fill immediately). Linear probing insert fails when
  /// size + tombstones >= max_load, even if logical count is low.
  /// Compaction is safe only when count == 0 (no live entries to preserve).
  void maybe_compact_outstanding() noexcept {
    if (outstanding_count_ == 0 && outstanding_.needs_rebuild()) {
      outstanding_.clear();
    }
  }

  /// Check if kill switch draining is complete after a cancel response.
  void check_drain_complete() noexcept {
    if (kill_switch_state_ == KillSwitchState::kDraining) {
      if (pending_cancel_count_ > 0) {
        --pending_cancel_count_;
      }
      if (pending_cancel_count_ == 0) {
        kill_switch_state_ = KillSwitchState::kComplete;
        sys::log::signal_log("[RISK] Kill switch complete, all cancels "
                             "resolved\n");
      }
    }
  }

  /// Clear active order tracking if the given order_id matches.
  /// Guards against clearing a slot that was already re-assigned to
  /// a newer order (e.g., old fill arriving after a new order was sent).
  void clear_active_if_match(std::uint32_t symbol_id, algo::Side side,
                             std::uint64_t client_order_id) const noexcept {
    if (symbol_id == 0 || symbol_id > ctx_.max_symbols) {
      return;
    }
    auto &active =
        ctx_.active_orders[((symbol_id - 1) * StrategyCtx::kSideCount) +
                           static_cast<std::size_t>(side)];
    if (active.resting && active.client_order_id == client_order_id) {
      active.resting = false;
    }
  }

  StrategyCtx &ctx_;

  std::uint64_t next_order_id_{1};
  std::uint32_t outstanding_count_{0};

  // -- Risk limits --
  std::int64_t max_position_;
  algo::Qty max_order_size_;
  std::int64_t max_notional_;

  // Order rate limiter — token bucket.
  // Tokens refill at a constant rate (one per refill_interval_ns_).
  // Each order consumes one token. Eliminates the boundary-burst problem
  // of fixed-window counters (up to 2x burst at window edges).
  // In real HFT, exchange-imposed rate limits are per-second or per-100ms
  // (e.g., CME: 50 msgs/sec, Nasdaq: configurable per port).
  std::uint32_t bucket_capacity_;      // Max tokens (= burst limit)
  std::int64_t refill_interval_ns_;    // Nanoseconds per token refill
  std::int64_t last_refill_ts_{0};     // Last refill timestamp
  std::uint32_t tokens_;               // Current available tokens

  // -- Exchange response counters --
  // Atomic for cross-thread monitoring safety (relaxed — zero overhead on
  // x86-64 where relaxed store/load compile to plain MOV instructions).
  std::atomic<std::uint64_t> acks_received_{0};
  std::atomic<std::uint64_t> rejects_received_{0};
  std::atomic<std::uint64_t> fills_received_{0};
  std::atomic<std::uint64_t> total_fill_qty_{0};
  std::atomic<std::uint64_t> modifies_sent_{0};
  std::atomic<std::uint64_t> modifies_acked_{0};
  std::atomic<std::uint64_t> modifies_rejected_{0};

  // -- Risk rejection counters (per-reason diagnostics) --
  std::atomic<std::uint64_t> risk_rejects_outstanding_{0};
  std::atomic<std::uint64_t> risk_rejects_per_symbol_{0};
  std::atomic<std::uint64_t> risk_rejects_position_{0};
  std::atomic<std::uint64_t> risk_rejects_size_{0};
  std::atomic<std::uint64_t> risk_rejects_notional_{0};
  std::atomic<std::uint64_t> risk_rejects_rate_{0};
  std::atomic<std::uint64_t> risk_rejects_killswitch_{0};

  // -- Kill switch state --
  KillSwitchState kill_switch_state_{KillSwitchState::kNormal};
  std::uint32_t pending_cancel_count_{0};

  // Outstanding orders — HashMap constructed from StrategyCtx buffer.
  OutstandingMap outstanding_;

  // -- Order timeout --
  // TimingWheel for automatic cancellation of unacked orders.
  // Constructed from StrategyCtx buffer (non-owning, caller-managed memory).
  ds::TimingWheel timeout_wheel_;
  std::int64_t order_timeout_ns_;
  std::int64_t last_advance_ns_{0};

  // Batch buffer for timeout callback — pointer into StrategyCtx.
  // on_order_timeout() writes client_order_ids here during tick().
  std::uint64_t *timeout_ids_;
  std::uint32_t timeout_count_{0};

  // Static instance pointer for callback (single-threaded, one OM per thread).
  inline static OrderManager *active_instance{nullptr};

  std::atomic<std::uint64_t> timeouts_fired_{0};
};

} // namespace mk::app
