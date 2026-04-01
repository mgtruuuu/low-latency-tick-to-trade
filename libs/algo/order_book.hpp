/**
 * @file order_book.hpp
 * @brief Zero-allocation limit order book for a single instrument.
 *
 * Data structure for maintaining resting limit orders organized by price
 * level, with O(1) cancel and O(1) best-bid/ask access.
 *
 * Wire format:
 *   This is a passive data structure — it stores and queries orders but does
 *   NOT perform matching (crossing). See matching_engine.hpp for that.
 *
 * Architecture:
 *   - Two sorted IntrusiveLists of PriceLevels (bids descending, asks
 *     ascending). Best price is always front().
 *   - Each PriceLevel holds an IntrusiveList<Order> in FIFO order (time
 *     priority within a price level).
 *   - HashMap<OrderId, Order*> for O(1) order lookup (cancel/modify).
 *   - HashMap<Price, PriceLevel*>[2] for O(1) price-to-level lookup.
 *   - Caller-managed buffer for Order/PriceLevel pools, IndexFreeStacks,
 *     and HashMaps. OrderBook NEVER owns memory — the caller allocates and
 *     frees the buffer. Use required_buffer_size() to compute allocation
 *     parameters, then pass a void* buffer to the constructor or create().
 *
 * Memory ownership:
 *   OrderBook follows the non-owning external storage principle used
 *   throughout this codebase (HashMap, IndexFreeStack, SPSCQueue,
 *   RingBuffer). The caller supplies a contiguous buffer; OrderBook
 *   partitions it internally into 7 sub-regions:
 *     1. Order pool           (max_orders × sizeof(Order))
 *     2. PriceLevel pool      (max_levels × sizeof(PriceLevel))
 *     3. Order free stack     (IndexFreeStack buffer)
 *     4. Level free stack     (IndexFreeStack buffer)
 *     5. Order HashMap        (order_map_cap slots)
 *     6. Bid level HashMap    (level_map_cap slots)
 *     7. Ask level HashMap    (level_map_cap slots)
 *
 * Design:
 *   - Zero allocation, no exceptions — suitable for hot-path use.
 *   - Single-threaded (no atomics, no locks).
 *   - All operations are noexcept. Failure returns false.
 *   - Order has a back-pointer to its PriceLevel for O(1) cancel without
 *     a second hash lookup.
 *   - Non-copyable, movable (move uses O(1) swap with IntrusiveList
 *     sentinel fixup — safe at startup, not during hot-path use).
 *
 * Hot path operations:
 *   - add_order:    O(1) amortized + O(k) for new price level insertion
 *                   (k = distance from best price, typically 1–5).
 *   - cancel_order: O(1) — hash lookup + intrusive list unlink.
 *   - modify_order: O(1) — hash lookup + in-place qty update.
 *   - best_bid/ask: O(1) — front of sorted list.
 *
 * Not implemented (production would need):
 *   - Multi-instrument: single symbol only. Real exchanges shard by symbol.
 *   - Price modification: only qty reduction supported (price change =
 *     cancel + new order, loses time priority).
 *   - Persistence / crash recovery: no WAL, snapshot, or event replay.
 *   - Market data publishing: no L2 (price/qty per level) or L3 (individual
 *     order) feed generation.
 *   - Audit trail: no sequence numbers or event log.
 */

#pragma once

#include "algo/trading_types.hpp"
#include "ds/hash_map.hpp"
#include "ds/index_free_stack.hpp"
#include "ds/intrusive_list.hpp"
#include "sys/bit_utils.hpp"
#include "sys/memory/arena_allocator.hpp" // Arena for bind() buffer partitioning

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring> // std::memset
#include <memory>  // std::construct_at, std::destroy_at
#include <optional>
#include <type_traits>

namespace mk::algo {

// Forward declaration — Order references PriceLevel, PriceLevel contains
// IntrusiveList<Order>. Both are defined in the same header.
struct PriceLevel;

// =============================================================================
// Order — a single resting limit order
// =============================================================================
//
// Inherits IntrusiveListHook for linkage into PriceLevel's FIFO order queue.
// Trivially destructible — no custom destructor, safe for pool reuse.
//
// Size on x86-64 (~48 bytes):
//   IntrusiveListHook: 16 (prev + next)
//   order_id:           8
//   price:              8
//   qty:                4
//   side:               1
//   (padding):          3
//   level:              8
//   Total:             48 — fits in one cache line.

struct Order : mk::ds::IntrusiveListHook {
  OrderId order_id{0};
  Price price{0};
  Qty qty{0};
  Side side{Side::kBid};
  PriceLevel *level{nullptr}; ///< Back-pointer to owning price level.
};

// Order lives in caller-managed storage without explicit placement-new.
// C++20 implicit-lifetime rules (P0593R6) guarantee that malloc/operator new
// implicitly create objects of implicit-lifetime type. mmap is not in the
// standard's list; this codebase assumes equivalent behavior on supported
// toolchains (implementation-defined). Order is an aggregate, so the pool
// can reuse slots without std::construct_at. The default member initializers
// ({0}, {nullptr}) make Order non-trivially-default-constructible, but it's
// still an aggregate and thus an implicit-lifetime type.
static_assert(std::is_aggregate_v<Order>,
              "Order must be an aggregate for implicit-lifetime pool reuse");
static_assert(std::is_trivially_destructible_v<Order>,
              "Order must be trivially destructible for pool reuse");

// =============================================================================
// PriceLevel — all orders at a single price
// =============================================================================
//
// Inherits IntrusiveListHook for linkage into the bid or ask sorted ladder.
// Contains IntrusiveList<Order> for the FIFO order queue at this price.
//
// Non-trivially destructible (IntrusiveList has a destructor that calls
// clear()). PriceLevels live in caller-managed storage and are
// placement-new'd at construction. OrderBook's destructor explicitly
// calls ~PriceLevel() before the caller frees the buffer.

struct PriceLevel : mk::ds::IntrusiveListHook {
  Price price{0};
  Qty total_qty{0};
  Side side{Side::kBid};
  mk::ds::IntrusiveList<Order> orders; ///< FIFO queue at this price.
};

// =============================================================================
// OrderBook — single-instrument limit order book (non-owning)
// =============================================================================

class OrderBook {
  // ---------------------------------------------------------------------------
  // Data members (HFT convention: layout visible first for cache analysis)
  // ---------------------------------------------------------------------------

  // Pools — pointers into caller-managed buffer.
  Order *order_slots_{nullptr};
  PriceLevel *level_slots_{nullptr};

  // Runtime capacities (for bounds checks in free_order/free_level).
  std::size_t max_orders_{0};
  std::size_t max_levels_{0};

  // Free index stacks — non-owning, stored in caller-managed buffer.
  mk::ds::IndexFreeStack order_free_;
  mk::ds::IndexFreeStack level_free_;

  // Sorted price ladders.
  mk::ds::IntrusiveList<PriceLevel> bids_; ///< Descending (best = front).
  mk::ds::IntrusiveList<PriceLevel> asks_; ///< Ascending (best = front).

  // Lookup maps — HashMap with caller-managed buffers.
  mk::ds::HashMap<OrderId, Order *> order_map_;

  // Price-to-level lookup: hash map (general-purpose), one per side.
  //
  // Alternative used in production for fixed-tick instruments:
  //   PriceLevel* level_by_tick_[MAX_TICKS];  // direct-indexed array
  // Convert price to tick index (price / tick_size), then array[tick_idx].
  // Eliminates hash computation + linear probing entirely — pure O(1) with
  // zero hash overhead. Requires bounded price range per instrument.
  //
  // We use HashMap because this is a general-purpose book (no assumption
  // about tick size or price range). For a single-instrument production book
  // with a known tick size, the direct-indexed array is strictly faster.
  mk::ds::HashMap<Price, PriceLevel *>
      level_map_[static_cast<int>(Side::kCount)];

public:
  /// Runtime capacity parameters (replaces former template parameters).
  struct Params {
    std::size_t max_orders{65536}; ///< Max live orders in the pool.
    std::size_t max_levels{4096};  ///< Max price-level slots in the pool.
    std::size_t order_map_cap{
        131072}; ///< 2× max_orders for ≤50% load factor (power of 2 >= 4).
    std::size_t level_map_cap{
        8192}; ///< 2× max_levels for ≤50% load factor (power of 2 >= 4).
  };

  // ===========================================================================
  // Static helpers
  // ===========================================================================

  /// Compute the required buffer size in bytes for the given params.
  /// Accounts for alignment padding between sub-regions.
  [[nodiscard]] static constexpr std::size_t
  required_buffer_size(const Params &params) noexcept {
    // Sub-region sizes (partition order matches bind()).
    const std::size_t order_pool = params.max_orders * sizeof(Order);
    const std::size_t level_pool = params.max_levels * sizeof(PriceLevel);
    const std::size_t order_free =
        mk::ds::IndexFreeStack::required_buffer_size(params.max_orders);
    const std::size_t level_free =
        mk::ds::IndexFreeStack::required_buffer_size(params.max_levels);

    using OrderMap = mk::ds::HashMap<OrderId, Order *>;
    using LevelMap = mk::ds::HashMap<Price, PriceLevel *>;
    const std::size_t order_map =
        OrderMap::required_buffer_size(params.order_map_cap);
    const std::size_t bid_map =
        LevelMap::required_buffer_size(params.level_map_cap);
    const std::size_t ask_map =
        LevelMap::required_buffer_size(params.level_map_cap);

    // Worst-case alignment padding: alignof(max) - 1 per sub-region boundary.
    // All sub-regions have alignment <= alignof(std::max_align_t) (16 on
    // x86-64). 6 internal boundaries × 15 = 90 bytes worst case.
    constexpr std::size_t kMaxPadding = 6 * (alignof(std::max_align_t) - 1);

    return order_pool + level_pool + order_free + level_free + order_map +
           bid_map + ask_map + kMaxPadding;
  }

  // ===========================================================================
  // Construction
  // ===========================================================================

  /// Default constructor — empty, unusable (no buffer bound).
  /// Exists for default-initialized arrays that are later move-assigned.
  OrderBook() noexcept = default;

  /// Direct constructor — binds to an external buffer, aborts on invalid input.
  /// Use this at startup time when failure is unrecoverable.
  OrderBook(void *buf, std::size_t buf_bytes, const Params &params) noexcept {
    if (!bind(buf, buf_bytes, params)) {
      std::abort();
    }
  }

  /// Factory — returns std::optional<OrderBook>. Preferred API for safe init.
  [[nodiscard]] static std::optional<OrderBook>
  create(void *buf, std::size_t buf_bytes, const Params &params) noexcept {
    OrderBook book;
    if (!book.bind(buf, buf_bytes, params)) {
      return std::nullopt;
    }
    return book; // NRVO — no move needed, but move ctor available as fallback.
  }

  // Non-copyable.
  OrderBook(const OrderBook &) = delete;
  OrderBook &operator=(const OrderBook &) = delete;

  // Movable — O(1) swap-based. IntrusiveList sentinel fixup handles
  // linked-node pointer updates. Safe only at startup before hot-path use.
  OrderBook(OrderBook &&other) noexcept { swap(other); }

  OrderBook &operator=(OrderBook &&other) noexcept {
    if (this != &other) {
      OrderBook tmp(std::move(other));
      swap(tmp); // tmp destructor cleans up our old state
    }
    return *this;
  }

  void swap(OrderBook &other) noexcept {
    std::swap(order_slots_, other.order_slots_);
    std::swap(level_slots_, other.level_slots_);
    std::swap(max_orders_, other.max_orders_);
    std::swap(max_levels_, other.max_levels_);
    order_free_.swap(other.order_free_);
    level_free_.swap(other.level_free_);
    bids_.swap(other.bids_);
    asks_.swap(other.asks_);
    order_map_.swap(other.order_map_);
    level_map_[side_index(Side::kBid)].swap(
        other.level_map_[side_index(Side::kBid)]);
    level_map_[side_index(Side::kAsk)].swap(
        other.level_map_[side_index(Side::kAsk)]);
  }

  friend void swap(OrderBook &a, OrderBook &b) noexcept { a.swap(b); }

  ~OrderBook() noexcept {
    clear();
    // Explicitly destroy PriceLevels before caller frees the buffer.
    // After clear(), all IntrusiveLists are empty, so destructors are cheap
    // (no-op clear). But skipping destructors for non-trivially-destructible
    // types is UB.
    if (level_slots_) {
      for (std::size_t i = 0; i < max_levels_; ++i) {
        std::destroy_at(&level_slots_[i]);
      }
    }
    // Orders are trivially destructible — no explicit destruction needed.
  }

  // ---------------------------------------------------------------------------
  // Modifiers
  // ---------------------------------------------------------------------------

  /// Add a resting limit order to the book.
  /// Production code never calls this directly — all orders enter through
  /// MatchingEngine::submit_order(), which calls rest_order() for unfilled
  /// remainders. Public for test and benchmark use (OrderBook in isolation).
  /// @return true on success, false if order pool exhausted, level pool
  ///         exhausted, order_map full, or duplicate order_id.
  [[nodiscard]] bool add_order(OrderId id, Side side, Price price,
                               Qty qty) noexcept {
    if (qty == 0) [[unlikely]] {
      return false;
    }

    // Reject duplicate order ID.
    if (order_map_.find(id) != nullptr) [[unlikely]] {
      return false;
    }

    // Allocate order from pool.
    Order *order = alloc_order();
    if (!order) [[unlikely]] {
      return false;
    }

    order->order_id = id;
    order->price = price;
    order->qty = qty;
    order->side = side;

    // Insert into order map.
    if (!order_map_.insert(id, order)) [[unlikely]] {
      free_order(order);
      return false;
    }

    // Find or create the price level.
    PriceLevel *level = find_or_create_level(side, price);
    if (!level) [[unlikely]] {
      order_map_.erase(id);
      free_order(order);
      return false;
    }

    // Append order to level's FIFO queue (time priority).
    order->level = level;
    level->orders.push_back(*order);
    level->total_qty += qty;

    return true;
  }

  /// Cancel (remove) an order by ID.
  /// @return true if the order existed and was removed.
  [[nodiscard]] bool cancel_order(OrderId id) noexcept {
    Order **pp = order_map_.find(id);
    if (!pp) [[unlikely]] {
      return false;
    }

    Order *order = *pp;
    PriceLevel *level = order->level;
    assert(level != nullptr);

    // Remove order from level's queue.
    level->orders.erase(*order);
    level->total_qty -= order->qty;

    // If level is now empty, remove it from the ladder and free it.
    if (level->orders.empty()) {
      remove_level(level);
    }

    // Remove from order map and free the order slot.
    order_map_.erase(id);
    free_order(order);

    return true;
  }

  /// Reduce an order's quantity. Does not lose time priority.
  /// If new_qty == 0, the order is cancelled.
  /// @return true if the order existed and was modified.
  [[nodiscard]] bool modify_order(OrderId id, Qty new_qty) noexcept {
    Order **pp = order_map_.find(id);
    if (!pp) [[unlikely]] {
      return false;
    }

    Order *order = *pp;

    // Only reduction is allowed — increasing qty would violate time priority.
    if (new_qty >= order->qty) [[unlikely]] {
      return false;
    }

    if (new_qty == 0) {
      // Inline cancellation — reuse already-found Order* to avoid a second
      // hash lookup that cancel_order(id) would perform.
      PriceLevel *level = order->level;
      assert(level != nullptr);

      level->orders.erase(*order);
      level->total_qty -= order->qty;

      if (level->orders.empty()) {
        remove_level(level);
      }

      order_map_.erase(id);
      free_order(order);
      return true;
    }

    const Qty delta = order->qty - new_qty;
    order->qty = new_qty;
    order->level->total_qty -= delta;

    return true;
  }

  /// Remove all orders and levels. Resets the book to empty state.
  void clear() noexcept {
    // Clear all active levels' order lists first, then free everything.
    // Walk bids and asks, clearing each level's order queue.
    auto clear_side = [&](mk::ds::IntrusiveList<PriceLevel> &ladder,
                          int side_idx) {
      while (!ladder.empty()) {
        PriceLevel &level = ladder.pop_front();
        while (!level.orders.empty()) {
          Order &order = level.orders.pop_front();
          free_order(&order);
        }
        free_level(&level);
      }
      level_map_[side_idx].clear();
    };

    clear_side(bids_, side_index(Side::kBid));
    clear_side(asks_, side_index(Side::kAsk));
    order_map_.clear();

    assert(bids_.empty() && "bids must be empty after clear");
    assert(asks_.empty() && "asks must be empty after clear");
    assert(total_orders() == 0 && "total_orders must be 0 after clear");
  }

  // ---------------------------------------------------------------------------
  // Queries
  // ---------------------------------------------------------------------------

  /// @return Best bid price, or nullopt if no bids.
  [[nodiscard]] std::optional<Price> best_bid() const noexcept {
    if (bids_.empty()) {
      return std::nullopt;
    }
    return bids_.front().price;
  }

  /// @return Best ask price, or nullopt if no asks.
  [[nodiscard]] std::optional<Price> best_ask() const noexcept {
    if (asks_.empty()) {
      return std::nullopt;
    }
    return asks_.front().price;
  }

  /// @return Spread (best_ask - best_bid), or nullopt if either side empty.
  [[nodiscard]] std::optional<Price> spread() const noexcept {
    auto bb = best_bid();
    auto ba = best_ask();
    if (!bb || !ba) {
      return std::nullopt;
    }
    return *ba - *bb;
  }

  /// @return Total quantity at the given price level, or 0 if no such level.
  [[nodiscard]] Qty volume_at_level(Side side, Price price) const noexcept {
    auto *const *pp = level_map_[side_index(side)].find(price);
    if (!pp) {
      return 0;
    }
    return (*pp)->total_qty;
  }

  /// @return Number of orders at the given price level.
  [[nodiscard]] std::uint32_t order_count_at_level(Side side,
                                                   Price price) const noexcept {
    auto *const *pp = level_map_[side_index(side)].find(price);
    if (!pp) {
      return 0;
    }
    return static_cast<std::uint32_t>((*pp)->orders.size());
  }

  /// @return true if an order with the given ID exists in the book.
  [[nodiscard]] bool has_order(OrderId id) const noexcept {
    return order_map_.find(id) != nullptr;
  }

  /// @return Remaining qty of the order, or 0 if not found (fully filled).
  [[nodiscard]] Qty order_qty(OrderId id) const noexcept {
    const auto *order_ptr = order_map_.find(id);
    return order_ptr ? (*order_ptr)->qty : 0;
  }

  /// @return Number of distinct price levels on the given side.
  [[nodiscard]] std::size_t book_depth(Side side) const noexcept {
    return side == Side::kBid ? bids_.size() : asks_.size();
  }

  /// @return Total number of resting orders in the book (both sides).
  [[nodiscard]] std::size_t total_orders() const noexcept {
    return order_map_.size();
  }

  // ---------------------------------------------------------------------------
  // Pool capacity observers (for testing / diagnostics)
  // ---------------------------------------------------------------------------

  [[nodiscard]] std::size_t free_order_count() const noexcept {
    return order_free_.available();
  }

  [[nodiscard]] std::size_t free_level_count() const noexcept {
    return level_free_.available();
  }

  // ---------------------------------------------------------------------------
  // Internals exposed for MatchingEngine (friend-level access)
  // ---------------------------------------------------------------------------

  /// @return Reference to the bid ladder (descending by price).
  [[nodiscard]] mk::ds::IntrusiveList<PriceLevel> &bids() noexcept {
    return bids_;
  }
  [[nodiscard]] const mk::ds::IntrusiveList<PriceLevel> &bids() const noexcept {
    return bids_;
  }

  /// @return Reference to the ask ladder (ascending by price).
  [[nodiscard]] mk::ds::IntrusiveList<PriceLevel> &asks() noexcept {
    return asks_;
  }
  [[nodiscard]] const mk::ds::IntrusiveList<PriceLevel> &asks() const noexcept {
    return asks_;
  }

  /// Remove a fully-filled order. Called by MatchingEngine during crossing.
  /// Handles level cleanup if the level becomes empty.
  /// @return true if the parent PriceLevel was destroyed (empty after removal).
  ///         Caller must not dereference the level after a true return.
  ///
  /// WARNING: This function frees the Order backing 'order', invalidating
  /// the reference and any iterator pointing to it. Do NOT call from a
  /// range-based for loop over a level's orders. Use a manual iterator
  /// loop and advance the iterator BEFORE calling this function:
  ///
  ///   for (auto it = level.orders.begin(); it != level.orders.end(); ) {
  ///     Order& order = *it;
  ///     ++it; // advance before invalidation
  ///     if (filled) { book.remove_filled_order(order); }
  ///   }
  [[nodiscard]] bool remove_filled_order(Order &order) noexcept {
    PriceLevel *level = order.level;
    assert(level != nullptr);

    level->orders.erase(order);
    level->total_qty -= order.qty;

    order_map_.erase(order.order_id);
    free_order(&order);

    if (level->orders.empty()) {
      remove_level(level);
      return true;
    }
    return false;
  }

  /// Add a resting order (used by MatchingEngine for unfilled remainder).
  /// Semantic alias for add_order — same validation, same behavior.
  /// Exists to make MatchingEngine call sites self-documenting:
  ///   book.rest_order(...)  // unfilled qty stays in the book
  /// vs
  ///   book.add_order(...)   // new incoming order
  [[nodiscard]] bool rest_order(OrderId id, Side side, Price price,
                                Qty qty) noexcept {
    return add_order(id, side, price, qty);
  }

private:
  // ---------------------------------------------------------------------------
  // Buffer binding (called by constructor and create() factory)
  // ---------------------------------------------------------------------------

  /// Partition the caller's buffer into 7 sub-regions.
  /// Returns false on validation failure (null buf, too small, bad params).
  [[nodiscard]] bool bind(void *buf, std::size_t buf_bytes,
                          const Params &params) noexcept {
    // Guard against re-bind (would leak PriceLevel destructor calls).
    if (order_slots_ != nullptr) {
      return false;
    }

    // Validate params.
    if (params.max_orders == 0 || params.max_levels == 0) {
      return false;
    }
    if (!mk::sys::is_power_of_two(
            static_cast<std::uint32_t>(params.order_map_cap)) ||
        params.order_map_cap < 4) {
      return false;
    }
    if (!mk::sys::is_power_of_two(
            static_cast<std::uint32_t>(params.level_map_cap)) ||
        params.level_map_cap < 4) {
      return false;
    }
    if (params.max_orders > params.order_map_cap) {
      return false;
    }
    if (params.max_levels > params.level_map_cap) {
      return false;
    }

    // Validate buffer.
    if (buf == nullptr || buf_bytes < required_buffer_size(params)) {
      return false;
    }

    // Store capacities for runtime bounds checks.
    max_orders_ = params.max_orders;
    max_levels_ = params.max_levels;

    // Partition the buffer into sub-regions using Arena as a bump allocator.
    mk::sys::memory::Arena arena(
        std::span<std::byte>(static_cast<std::byte *>(buf), buf_bytes));

    // 1. Order pool — zero-initialize for IntrusiveListHook safety.
    order_slots_ = arena.alloc<Order>(params.max_orders);
    if (!order_slots_) {
      return false;
    }
    std::memset(order_slots_, 0, max_orders_ * sizeof(Order));

    // 2. PriceLevel pool — placement-construct for IntrusiveList sentinel init.
    level_slots_ = arena.alloc<PriceLevel>(params.max_levels);
    if (!level_slots_) {
      order_slots_ = nullptr;
      return false;
    }
    for (std::size_t i = 0; i < max_levels_; ++i) {
      std::construct_at(&level_slots_[i]);
    }

    // 3. Order free stack.
    const auto ofs_size =
        mk::ds::IndexFreeStack::required_buffer_size(params.max_orders);
    auto *ofs_buf = arena.alloc(ofs_size, alignof(std::uint32_t));
    auto order_free_opt = ofs_buf ? mk::ds::IndexFreeStack::create(
                                        ofs_buf, ofs_size, params.max_orders)
                                  : std::nullopt;
    if (!order_free_opt) {
      order_slots_ = nullptr;
      return false;
    }
    order_free_ = std::move(*order_free_opt);

    // 4. Level free stack.
    const auto lfs_size =
        mk::ds::IndexFreeStack::required_buffer_size(params.max_levels);
    auto *lfs_buf = arena.alloc(lfs_size, alignof(std::uint32_t));
    auto level_free_opt = lfs_buf ? mk::ds::IndexFreeStack::create(
                                        lfs_buf, lfs_size, params.max_levels)
                                  : std::nullopt;
    if (!level_free_opt) {
      order_slots_ = nullptr;
      return false;
    }
    level_free_ = std::move(*level_free_opt);

    // 5. Order HashMap.
    using OrderMap = mk::ds::HashMap<OrderId, Order *>;
    const auto om_size = OrderMap::required_buffer_size(params.order_map_cap);
    auto *om_buf = arena.alloc(om_size, OrderMap::slot_alignment());
    auto order_map_opt =
        om_buf ? OrderMap::create(om_buf, om_size, params.order_map_cap)
               : std::nullopt;
    if (!order_map_opt) {
      order_slots_ = nullptr;
      return false;
    }
    order_map_ = std::move(*order_map_opt);

    // 6. Bid level HashMap.
    using LevelMap = mk::ds::HashMap<Price, PriceLevel *>;
    const auto lm_size = LevelMap::required_buffer_size(params.level_map_cap);
    auto *bid_buf = arena.alloc(lm_size, LevelMap::slot_alignment());
    auto bid_map_opt =
        bid_buf ? LevelMap::create(bid_buf, lm_size, params.level_map_cap)
                : std::nullopt;
    if (!bid_map_opt) {
      order_slots_ = nullptr;
      return false;
    }
    level_map_[side_index(Side::kBid)] = std::move(*bid_map_opt);

    // 7. Ask level HashMap.
    auto *ask_buf = arena.alloc(lm_size, LevelMap::slot_alignment());
    auto ask_map_opt =
        ask_buf ? LevelMap::create(ask_buf, lm_size, params.level_map_cap)
                : std::nullopt;
    if (!ask_map_opt) {
      order_slots_ = nullptr;
      return false;
    }
    level_map_[side_index(Side::kAsk)] = std::move(*ask_map_opt);

    return true;
  }

  /// Convert Side enum to array index with bounds check.
  /// Side is uint8_t with values 0/1 — this guards against invalid casts.
  static constexpr int side_index(Side s) noexcept {
    const int idx = static_cast<int>(s);
    assert(idx >= 0 && idx <= 1 && "Invalid Side enum value");
    return idx;
  }

  // ---------------------------------------------------------------------------
  // Pool management (IndexFreeStack + caller-managed slots)
  // ---------------------------------------------------------------------------
  //
  // Each pool is a caller-managed array of pre-allocated slots plus an
  // IndexFreeStack managing free slot indices.
  //
  // Allocate: pop index from free stack, return &slots_[index].
  // Deallocate: compute index from pointer arithmetic, push to free stack.
  //
  // This is the simplest possible single-threaded pool — no lock-free
  // overhead, no alignment constraints beyond T's natural alignment.

  [[nodiscard]] Order *alloc_order() noexcept {
    std::uint32_t idx = 0;
    if (!order_free_.pop(idx)) [[unlikely]] {
      return nullptr;
    }
    return &order_slots_[idx];
  }

  void free_order(Order *order) noexcept {
    assert(order >= &order_slots_[0] && order < &order_slots_[0] + max_orders_);
    auto idx = static_cast<std::uint32_t>(order - &order_slots_[0]);
    // Reset the order's hook state so it's cleanly reusable.
    order->prev = nullptr;
    order->next = nullptr;
    order->level = nullptr;
    order_free_.push(idx);
  }

  [[nodiscard]] PriceLevel *alloc_level() noexcept {
    std::uint32_t idx = 0;
    if (!level_free_.pop(idx)) [[unlikely]] {
      return nullptr;
    }
    return &level_slots_[idx];
  }

  void free_level(PriceLevel *level) noexcept {
    assert(level >= &level_slots_[0] && level < &level_slots_[0] + max_levels_);
    // Defensive: freeing a level that still has orders would leak them.
    // remove_level() already checks this, but guard against direct misuse.
    assert(level->orders.empty() &&
           "Attempted to free a PriceLevel that still contains orders");
    auto idx = static_cast<std::uint32_t>(level - &level_slots_[0]);
    // Reset the level's hook state.
    level->prev = nullptr;
    level->next = nullptr;
    level_free_.push(idx);
  }

  // ---------------------------------------------------------------------------
  // Level management
  // ---------------------------------------------------------------------------

  /// Find an existing price level or create a new one.
  /// New levels are inserted into the sorted ladder at the correct position.
  [[nodiscard]] PriceLevel *find_or_create_level(Side side,
                                                 Price price) noexcept {
    const int si = side_index(side);

    // Fast path: level already exists.
    PriceLevel **pp = level_map_[si].find(price);
    if (pp) {
      return *pp;
    }

    // Allocate a new level.
    PriceLevel *level = alloc_level();
    if (!level) [[unlikely]] {
      return nullptr;
    }

    level->price = price;
    level->total_qty = 0;
    level->side = side;
    // level->orders is already default-constructed (empty IntrusiveList).

    // Insert into the level map.
    // Logically unreachable: max_levels <= level_map_cap (enforced by bind()),
    // and alloc_level() above would fail before the map runs out of slots.
    // Duplicate key is also impossible — find() returned nullptr above.
    // Defensive check retained for safety.
    if (!level_map_[si].insert(price, level)) [[unlikely]] {
      free_level(level);
      return nullptr;
    }

    // Insert into the sorted ladder at the correct position.
    insert_level_sorted(side, *level);

    return level;
  }

  /// Insert a price level into the bid or ask ladder in sorted order.
  ///
  /// Bids: descending by price (highest = front, best bid).
  ///   Walk from front; insert before the first level with price < new price.
  ///
  /// Asks: ascending by price (lowest = front, best ask).
  ///   Walk from front; insert before the first level with price > new price.
  ///
  /// Complexity: O(k) where k = distance from best price. New orders tend to
  /// cluster near the top of book, so k is typically 1–5 in practice.
  void insert_level_sorted(Side side, PriceLevel &level) noexcept {
    auto &ladder = (side == Side::kBid) ? bids_ : asks_;

    if (ladder.empty()) {
      ladder.push_back(level);
      return;
    }

    if (side == Side::kBid) {
      // Descending: find first existing level with price < level.price.
      for (auto it = ladder.begin(); it != ladder.end(); ++it) {
        if (it->price < level.price) {
          ladder.insert_before(*it, level);
          return;
        }
      }
      // New level has the lowest price — append to back.
      ladder.push_back(level);
    } else {
      // Ascending: find first existing level with price > level.price.
      for (auto it = ladder.begin(); it != ladder.end(); ++it) {
        if (it->price > level.price) {
          ladder.insert_before(*it, level);
          return;
        }
      }
      // New level has the highest price — append to back.
      ladder.push_back(level);
    }
  }

  /// Remove an empty price level from the ladder, level map, and free pool.
  void remove_level(PriceLevel *level) noexcept {
    assert(level->orders.empty());

    const int si = side_index(level->side);
    auto &ladder = (level->side == Side::kBid) ? bids_ : asks_;

    ladder.erase(*level);
    level_map_[si].erase(level->price);
    free_level(level);
  }
};

} // namespace mk::algo
