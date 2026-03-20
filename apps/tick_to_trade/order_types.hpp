/**
 * @file order_types.hpp
 * @brief Order management shared types (OrderInfo, ActiveOrder, OutstandingMap).
 *
 * These types are used by StrategyCtx (for buffer layout) and OrderManager
 * (for runtime state). Extracted here to avoid circular includes between
 * strategy_ctx.hpp and order_manager.hpp.
 */

#pragma once

#include "algo/trading_types.hpp" // algo::Side, algo::Price, algo::Qty

#include "ds/hash_map.hpp"
#include "ds/timing_wheel.hpp"

#include <cstddef>
#include <cstdint>

namespace mk::app {

/// Info stored per outstanding order. Keyed by client_order_id in the
/// outstanding order HashMap. FillReport lacks symbol_id, so we store it
/// here for fill-time position updates.
struct OrderInfo {
  algo::Side side;
  algo::Price price;
  algo::Qty qty;
  std::uint32_t symbol_id;
  std::int64_t send_ts;
  ds::TimingWheel::handle_t timer_handle; // for timeout cancel on fill/reject
};

/// Tracks one resting order per (symbol, side) for modify-or-new logic.
/// When a signal arrives for a (symbol, side) that already has a resting
/// order, the OMS sends a ModifyOrder instead of cancel + new.
struct ActiveOrder {
  std::uint64_t client_order_id{0};
  algo::Price price{0};
  algo::Qty qty{0};
  bool resting{false};
};

/// HashMap type for outstanding order tracking.
using OutstandingMap = ds::HashMap<std::uint64_t, OrderInfo>;

/// Compute HashMap capacity for a given max_outstanding count.
/// Uses 4x multiplier (not 2x) to handle tombstone accumulation in
/// high-churn scenarios (e.g. replay mode where orders fill immediately).
/// With 2x: capacity=64, max_load=44. Rapid insert→erase cycles accumulate
/// tombstones quickly, causing insert to fail when size+tombstones >= 44.
/// With 4x: capacity=128, max_load=89. Tombstones can accumulate to 57
/// before impacting inserts, which is far more than max_outstanding=32.
[[nodiscard]] constexpr std::size_t
order_ctx_map_capacity(std::uint32_t max_outstanding) noexcept {
  return OutstandingMap::round_up_capacity(
      static_cast<std::size_t>(max_outstanding) * 4);
}

} // namespace mk::app
