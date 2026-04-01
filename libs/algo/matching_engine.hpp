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
#include <optional>
#include <span>
#include <utility> // std::move, std::swap

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
/// valid only until the next call to submit_order() or modify_order() on
/// the same engine instance — the buffer is overwritten each time. Moving
/// or swapping the engine also invalidates any outstanding span (fills_
/// is swapped to a different object). Process fills before any of these
/// operations. This is the standard HFT pattern (analogous to
/// epoll_wait's event buffer).
///
// clang-format off
/// Observable outcomes (fills, remaining_qty, rested):
///   !empty, remaining == 0, !rested → fully filled
///   !empty, remaining >  0,  rested → partial fill, rested
///   !empty, remaining >  0, !rested → partial fill, NOT rested
///    empty, remaining >  0,  rested → no match, rested
///    empty, remaining >  0, !rested → no fill, not rested
///    empty, remaining == 0, !rested → zero-qty (rejected)
///
/// Root causes NOT distinguishable from fields alone:
///   "no fill, not rested" → duplicate ID or book full.
///   "partial fill, NOT rested" → buffer exhaustion or book full.
// clang-format on
struct MatchResult {
  std::span<const Fill> fills; ///< Fills generated (volatile — see above).
  Qty remaining_qty{0};        ///< Unfilled portion (0 = fully filled).
  bool rested{false};          ///< True if remainder was rested in the book.

  // Helpers — decode the 3-field combination without memorizing the table.
  [[nodiscard]] bool has_fills() const noexcept { return !fills.empty(); }
  [[nodiscard]] bool fully_filled() const noexcept {
    return !fills.empty() && remaining_qty == 0 && !rested;
  }
  [[nodiscard]] bool rested_in_book() const noexcept { return rested; }
  [[nodiscard]] bool rejected() const noexcept {
    return fills.empty() && !rested;
  }
};

// =============================================================================
// ModifyResult — outcome of a modify_order() call
// =============================================================================

/// Outcome of a modify_order() call (cancel + re-submit).
///
/// Lifetime: same as MatchResult — `fills` span is valid only until the
/// next submit_order(), modify_order(), move, or swap (shares the same
/// fill buffer).
///
// clang-format off
/// Outcome decoding:
///   success == false: original order not found (cancel failed).
///   success == true:  cancel succeeded, replacement went through submit_order().
///                     Observable outcomes same as MatchResult above.
// clang-format on
struct ModifyResult {
  std::span<const Fill> fills; ///< Fills if new price crosses opposite side.
  Qty remaining_qty{0};        ///< Unfilled portion after re-submission.
  bool rested{false};          ///< True if remainder was rested in the book.
  bool success{false};         ///< False if original order was not found.

  [[nodiscard]] bool rested_in_book() const noexcept {
    return success && rested;
  }
};

// =============================================================================
// MatchingEngine
// =============================================================================
//
// MaxFills is the only remaining template parameter (controls inline fill
// buffer size). OrderBook capacity is runtime via OrderBook::Params.

template <std::size_t MaxFills = 64> class MatchingEngine {
  // ---------------------------------------------------------------------------
  // Data members
  // ---------------------------------------------------------------------------
  OrderBook book_;
  std::array<Fill, MaxFills> fills_{};
  std::uint32_t fill_count_{0};

public:
  static constexpr std::size_t kMaxFillsPerMatch = MaxFills;
  using Book = OrderBook;

  /// Forward OrderBook::required_buffer_size for caller convenience.
  [[nodiscard]] static constexpr std::size_t
  required_buffer_size(const OrderBook::Params &params) noexcept {
    return OrderBook::required_buffer_size(params);
  }

  /// Factory — returns std::optional<MatchingEngine>.
  [[nodiscard]] static std::optional<MatchingEngine>
  create(void *buf, std::size_t buf_bytes,
         const OrderBook::Params &params) noexcept {
    auto book = OrderBook::create(buf, buf_bytes, params);
    if (!book) {
      return std::nullopt;
    }
    return MatchingEngine(std::move(*book));
  }

  /// Default constructor — empty engine (OrderBook is default-constructed,
  /// unusable). Exists for default-initialized arrays / two-phase patterns.
  MatchingEngine() noexcept = default;

  /// Construct from a ready OrderBook (takes ownership via move).
  explicit MatchingEngine(OrderBook &&book) noexcept : book_(std::move(book)) {}

  /// Direct constructor — forwards to OrderBook constructor.
  /// Aborts if the buffer is invalid.
  MatchingEngine(void *buf, std::size_t buf_bytes,
                 const OrderBook::Params &params) noexcept
      : book_(buf, buf_bytes, params) {}

  ~MatchingEngine() = default;

  // Non-copyable.
  MatchingEngine(const MatchingEngine &) = delete;
  MatchingEngine &operator=(const MatchingEngine &) = delete;

  // Movable — swap-based, consistent with OrderBook/HashMap/IntrusiveList.
  // fills_ (1.5KB) is included in the swap. Moves only happen at startup
  // so the cost is irrelevant.
  MatchingEngine(MatchingEngine &&other) noexcept { swap(other); }

  MatchingEngine &operator=(MatchingEngine &&other) noexcept {
    if (this != &other) {
      MatchingEngine tmp(std::move(other));
      swap(tmp);
    }
    return *this;
  }

  void swap(MatchingEngine &other) noexcept {
    book_.swap(other.book_);
    std::swap(fills_, other.fills_);
    std::swap(fill_count_, other.fill_count_);
  }

  friend void swap(MatchingEngine &a, MatchingEngine &b) noexcept { a.swap(b); }

  /// Submit a limit order. Crosses resting orders if the price overlaps,
  /// then rests any unfilled remainder.
  ///
  /// @return MatchResult with fills and remaining quantity. The fills span
  ///         is valid only until the next submit_order(), modify_order(),
  ///         move, or swap (they share the same fills_[] buffer).
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
  /// Success contract: `success` means the old order was found and
  /// cancelled. The replacement order then goes through submit_order()
  /// which may itself fail (zero qty, duplicate new_id, book full) —
  /// those outcomes are reflected in fills/remaining_qty/rested, NOT in
  /// the success flag. This matches FIX semantics where the cancel and
  /// the replacement are logically separate operations.
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
};

} // namespace mk::algo
