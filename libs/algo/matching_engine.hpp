/**
 * @file matching_engine.hpp
 * @brief Order matching engine with price-time priority crossing.
 *
 * Wraps an OrderBook and adds crossing logic: when an incoming order's
 * price overlaps resting orders on the opposite side, fills are generated.
 *
 * See order_book.hpp for the underlying data structure.
 *
 * Not implemented (production would need):
 *   - Order types: only limit orders. No market, IOC (Immediate-Or-Cancel),
 *     FOK (Fill-Or-Kill), GTC/GTD, stop orders.
 *   - Self-trade prevention: no check for same-participant crossing.
 *   - Pre-trade risk: no order size limits, price band checks, or fat
 *     finger protection.
 *   - Opening/closing auction: no call auction mechanism.
 *   - Circuit breakers: no price band halt logic.
 */

#pragma once

#include "algo/order_book.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mk::algo {

// =============================================================================
// Fill — a single trade execution
// =============================================================================

struct Fill {
  OrderId maker_id{0}; ///< Resting (passive) order that was matched.
  OrderId taker_id{0}; ///< Incoming (aggressive) order.
  Price price{0};      ///< Execution price (always the maker's price).
  Qty qty{0};          ///< Quantity filled.
};

// =============================================================================
// MatchResult — outcome of a submit_order() call
// =============================================================================

/// Outcome of a submit_order() call.
///
/// Lifetime: `fills` is a span into the engine's internal buffer. It is
/// valid only until the next call to submit_order() on the same engine
/// instance — the buffer is overwritten each time. Process fills before
/// submitting the next order. This is the standard HFT pattern (analogous
/// to epoll_wait's event buffer).
///
/// Outcome decoding (all cases distinguishable from these fields):
///   Fully filled:          remaining_qty == 0, rested == false, !fills.empty()
///   Partial fill + rested: remaining_qty > 0,  rested == true
///   No match, rested:      remaining_qty > 0,  rested == true,  fills.empty()
///   Zero-qty rejected:     remaining_qty == 0, rested == false, fills.empty()
///   Duplicate ID rejected: remaining_qty > 0,  rested == false, fills.empty()
///   Book full (can't rest): remaining_qty > 0, rested == false
///   Fill buffer exhausted:  remaining_qty > 0, rested == false, !fills.empty()
struct MatchResult {
  std::span<const Fill> fills; ///< Fills generated (volatile — see above).
  Qty remaining_qty{0};        ///< Unfilled portion (0 = fully filled).
  bool rested{false};          ///< True if remainder was rested in the book.
};

// =============================================================================
// ModifyResult — outcome of a modify_order() call
// =============================================================================

/// Outcome of a modify_order() call (cancel + re-submit).
///
/// Lifetime: same as MatchResult — `fills` span is valid only until the
/// next submit_order() or modify_order() call (shares the same fill buffer).
struct ModifyResult {
  std::span<const Fill> fills; ///< Fills if new price crosses opposite side.
  Qty remaining_qty{0};        ///< Unfilled portion after re-submission.
  bool rested{false};          ///< True if remainder was rested in the book.
  bool success{false};         ///< False if original order was not found.
};

// =============================================================================
// MatchingEngine
// =============================================================================
//
// Template parameters mirror OrderBook plus MaxFills (max fills per match).
// The fills buffer is a fixed array — no allocation during matching.

template <std::size_t MaxFills = 64, std::size_t MaxOrders = 65536,
          std::size_t MaxLevels = 4096, std::size_t OrderMapCap = 65536,
          std::size_t LevelMapCap = 8192>
class MatchingEngine {
public:
  static constexpr std::size_t kMaxFillsPerMatch = MaxFills;
  using Book = OrderBook<MaxOrders, MaxLevels, OrderMapCap, LevelMapCap>;

  MatchingEngine() = default;
  ~MatchingEngine() = default;

  // Non-copyable, non-movable (underlying OrderBook contains large arrays
  // and IntrusiveLists with internal pointers).
  MatchingEngine(const MatchingEngine &) = delete;
  MatchingEngine &operator=(const MatchingEngine &) = delete;
  MatchingEngine(MatchingEngine &&) = delete;
  MatchingEngine &operator=(MatchingEngine &&) = delete;

  /// Submit a limit order. Crosses resting orders if the price overlaps,
  /// then rests any unfilled remainder.
  ///
  /// @return MatchResult with fills and remaining quantity. The fills span
  ///         is valid only until the next submit_order() call.
  [[nodiscard]] MatchResult submit_order(OrderId id, Side side, Price price,
                                         Qty qty) noexcept {
    // Reject zero-quantity orders upfront. Without this, qty=0 bypasses
    // matching and reaches rest_order() which rejects it, but returns
    // {remaining_qty: 0, rested: false} — ambiguous with a fully-filled order.
    if (qty == 0) [[unlikely]] {
      return MatchResult{
          .fills = std::span<const Fill>(fills_.data(), 0),
          .remaining_qty = 0,
          .rested = false,
      };
    }

    // Reject duplicate order ID before matching. Without this, an incoming
    // order could match against a resting order with the same ID, producing
    // fills where maker_id == taker_id and breaking the unique-order-id
    // invariant that downstream systems rely on.
    if (book_.has_order(id)) [[unlikely]] {
      return MatchResult{
          .fills = std::span<const Fill>(fills_.data(), 0),
          .remaining_qty = qty,
          .rested = false,
      };
    }

    fill_count_ = 0;
    Qty remaining = qty;

    // Determine the opposite side's ladder.
    auto &opposite_ladder = (side == Side::kBid) ? book_.asks() : book_.bids();

    // Cross resting orders while the price overlaps.
    // Stop matching if fill buffer is exhausted — never silently drop fills.
    // The unfilled remainder will rest in the book with accurate remaining_qty.
    while (remaining > 0 && !opposite_ladder.empty() &&
           fill_count_ < MaxFills) {
      PriceLevel &best_level = opposite_ladder.front();

      // Check if the incoming order's price crosses the best resting price.
      // Buy crosses if price >= best ask. Sell crosses if price <= best bid.
      const bool crosses = (side == Side::kBid) ? (price >= best_level.price)
                                                : (price <= best_level.price);
      if (!crosses) {
        break;
      }

      // Match against orders at this level in FIFO order.
      // Cache the execution price — best_level reference becomes invalid
      // if remove_filled_order() empties and frees the level.
      const Price exec_price = best_level.price;

      while (remaining > 0 && !best_level.orders.empty() &&
             fill_count_ < MaxFills) {
        Order &resting = best_level.orders.front();
        const Qty fill_qty = std::min(remaining, resting.qty);

        // Record the fill.
        fills_[fill_count_++] = Fill{
            .maker_id = resting.order_id,
            .taker_id = id,
            .price = exec_price, // Execute at resting price.
            .qty = fill_qty,
        };

        remaining -= fill_qty;
        resting.qty -= fill_qty;
        best_level.total_qty -= fill_qty;

        if (resting.qty == 0) {
          // Fully filled resting order — remove from book.
          // remove_filled_order returns true if the level was destroyed,
          // invalidating best_level. Break to re-fetch from outer loop.
          if (book_.remove_filled_order(resting)) {
            break;
          }
        }
      }
    }

    // Rest unfilled remainder in the book — but only if it would not create
    // a crossed book. When matching stopped due to fill buffer exhaustion
    // (not because we ran out of crossing levels), the opposite side still
    // has orders at overlapping prices. Resting would place a bid >= best ask
    // (or ask <= best bid), which is an invalid book state. In that case,
    // leave the remainder unhandled and let the caller deal with it.
    bool rested = false;
    if (remaining > 0) {
      // Check if matching stopped due to fill buffer exhaustion (still has
      // overlapping prices) vs price gap (no more crossing levels).
      // Only the former produces a crossed book if we rest the remainder.
      const bool still_crosses =
          !opposite_ladder.empty() &&
          ((side == Side::kBid) ? (price >= opposite_ladder.front().price)
                                : (price <= opposite_ladder.front().price));
      if (!still_crosses) {
        rested = book_.rest_order(id, side, price, remaining);
      }
    }

    return MatchResult{
        .fills = std::span<const Fill>(fills_.data(), fill_count_),
        .remaining_qty = remaining,
        .rested = rested,
    };
  }

  /// Cancel an existing resting order.
  [[nodiscard]] bool cancel_order(OrderId id) noexcept {
    return book_.cancel_order(id);
  }

  /// Modify a resting order's price and/or quantity.
  ///
  /// Implemented as cancel + re-submit (loses time priority), matching
  /// real exchange behavior: FIX OrderCancelReplaceRequest (MsgType=G),
  /// ITCH Order Replace (Type U). The modified order gets a new
  /// exchange-assigned ID (new_id).
  ///
  /// Side is required because cancel_order() does not return the old
  /// order's side — the gateway must look it up from its id mapping.
  ///
  /// @return ModifyResult with success=false if the original order was
  ///         not found (already cancelled/filled).
  [[nodiscard]] ModifyResult modify_order(OrderId old_id, Side side,
                                          Price new_price, Qty new_qty,
                                          OrderId new_id) noexcept {
    if (!book_.cancel_order(old_id)) {
      return ModifyResult{
          .fills = std::span<const Fill>(fills_.data(), 0),
          .remaining_qty = 0,
          .rested = false,
          .success = false,
      };
    }
    auto result = submit_order(new_id, side, new_price, new_qty);
    return ModifyResult{
        .fills = result.fills,
        .remaining_qty = result.remaining_qty,
        .rested = result.rested,
        .success = true,
    };
  }

  /// Direct access to the underlying order book for queries.
  [[nodiscard]] const Book &book() const noexcept { return book_; }

private:
  Book book_;
  std::array<Fill, MaxFills> fills_{};
  std::uint32_t fill_count_{0};
};

} // namespace mk::algo
