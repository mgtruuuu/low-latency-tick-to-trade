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
 *     MmapRegion-backed (externally managed buffer, not inline std::array).
 *   - HashMap<Price, PriceLevel*>[2] for O(1) price-to-level lookup.
 *     Each side's map is a separate MmapRegion.
 *   - MmapRegion-backed pools (huge page ready, NUMA-aware) for Order and
 *     PriceLevel with free index stacks. Zero allocation on the hot path.
 *
 * Design:
 *   - Zero allocation, no exceptions — suitable for hot-path use.
 *   - Single-threaded (no atomics, no locks).
 *   - All operations are noexcept. Failure returns false.
 *   - Order has a back-pointer to its PriceLevel for O(1) cancel without
 *     a second hash lookup.
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
#include "ds/fixed_index_free_stack.hpp"
#include "ds/hash_map.hpp"
#include "ds/intrusive_list.hpp"
#include "sys/memory/mmap_utils.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>  // std::construct_at, std::destroy_at
#include <type_traits> // std::is_trivially_default_constructible_v

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

// Order lives in MmapRegion-backed storage without explicit placement-new.
// C++20 implicit-lifetime rules (P0593R6) apply to aggregates: mmap/memset
// implicitly creates objects of implicit-lifetime type. Order is an aggregate
// (no user-declared constructors), so the pool can reuse slots without
// std::construct_at. The default member initializers ({0}, {nullptr}) make
// Order non-trivially-default-constructible, but it's still an aggregate
// and thus an implicit-lifetime type.
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
// clear()). PriceLevels live in MmapRegion-backed storage and are
// placement-new'd at construction. OrderBook's destructor explicitly
// calls ~PriceLevel() before MmapRegion unmaps the memory.

struct PriceLevel : mk::ds::IntrusiveListHook {
  Price price{0};
  Qty total_qty{0};
  std::uint32_t order_count{0};
  Side side{Side::kBid};
  mk::ds::IntrusiveList<Order> orders; ///< FIFO queue at this price.
};

// =============================================================================
// OrderBook — single-instrument limit order book
// =============================================================================

/// @tparam MaxOrders    Max live orders in the pool (shared across both sides).
/// @tparam MaxLevels    Max price-level slots in the pool (shared across both
/// sides).
/// @tparam OrderMapCap  HashMap capacity for order lookup (one map,
/// MmapRegion-backed).
///                      Must be a power of two ≥ 4.
/// @tparam LevelMapCap  HashMap capacity for each per-side price→level map
///                      (MmapRegion-backed). Default is 2× MaxLevels to keep
///                      load factor ≤50% even if all levels land on one side —
///                      well under the 70% limit, giving short probe chains for
///                      linear probing.
template <std::size_t MaxOrders = 65536, std::size_t MaxLevels = 4096,
          std::size_t OrderMapCap = 65536, std::size_t LevelMapCap = 8192>
class OrderBook {
  // Validate template parameters at compile time. HashMap requires
  // power-of-two capacity >= 4 for correct linear probing (masking).
  // Runtime abort in HashMap constructor would catch this too, but
  // static_assert gives a clear error message at instantiation time.
  static_assert(MaxOrders > 0, "MaxOrders must be > 0");
  static_assert(MaxLevels > 0, "MaxLevels must be > 0");
  static_assert(
      OrderMapCap >= 4 && (OrderMapCap & (OrderMapCap - 1)) == 0,
      "OrderMapCap must be a power of two >= 4 (HashMap requirement)");
  static_assert(
      LevelMapCap >= 4 && (LevelMapCap & (LevelMapCap - 1)) == 0,
      "LevelMapCap must be a power of two >= 4 (HashMap requirement)");

public:
  // Convenience constructor — two-level huge page fallback.
  // Aborts if memory allocation fails (startup-time use).
  //
  // @param pf         Prefault policy (default: kPopulateWrite — zero hot-path
  // faults).
  // @param numa_node  NUMA node to bind memory to (-1 = no binding).
  // @param lock_pages If true, pin pages in RAM via MAP_LOCKED.
  explicit OrderBook(mk::sys::memory::PrefaultPolicy pf =
                         mk::sys::memory::PrefaultPolicy::kPopulateWrite,
                     int numa_node = -1, bool lock_pages = false) noexcept {

    using namespace mk::sys::memory;

    const auto alloc_region = [&](std::size_t bytes) -> MmapRegion {
      const RegionIntentConfig cfg{
          .size = bytes, .numa_node = numa_node, .lock_pages = lock_pages};
      RegionIntent intent = RegionIntent::kHotRw; // Defensive default.
      switch (pf) {
      case PrefaultPolicy::kPopulateWrite:
        intent = RegionIntent::kHotRw;
        break;
      case PrefaultPolicy::kPopulateRead:
        intent = RegionIntent::kReadMostly;
        break;
      case PrefaultPolicy::kNone:
        intent = RegionIntent::kCold;
        break;
      case PrefaultPolicy::kManualWrite:
        intent = RegionIntent::kHotRw; // Manual prefault → degrade to hot R/W.
        break;
      case PrefaultPolicy::kManualRead:
        intent = RegionIntent::kReadMostly; // Manual prefault → degrade to read.
        break;
      }
      return allocate_region(cfg, intent);
    };

    // --- Allocate Order pool ---
    order_memory_ = alloc_region(MaxOrders * sizeof(Order));
    order_slots_ = static_cast<Order *>(order_memory_.get());  // NOLINT(cppcoreguidelines-prefer-member-initializer)

    // --- Allocate PriceLevel pool ---
    level_memory_ = alloc_region(MaxLevels * sizeof(PriceLevel));
    level_slots_ = static_cast<PriceLevel *>(level_memory_.get());  // NOLINT(cppcoreguidelines-prefer-member-initializer)

    // --- Allocate order_map buffer (HashMap, externally backed) ---
    using OrderMap = mk::ds::HashMap<OrderId, Order *>;
    order_map_memory_ = alloc_region(OrderMap::required_buffer_size(OrderMapCap));
    order_map_memory_.advise(MADV_RANDOM);
    order_map_ = OrderMap(order_map_memory_.get(), order_map_memory_.size(),
                          OrderMapCap);

    // --- Allocate level_map buffers (one per side) ---
    using LevelMap = mk::ds::HashMap<Price, PriceLevel *>;
    for (int i = 0; i < 2; ++i) {
      level_map_memory_[i] =
          alloc_region(LevelMap::required_buffer_size(LevelMapCap));
      level_map_memory_[i].advise(MADV_RANDOM);
      level_map_[i] = LevelMap(level_map_memory_[i].get(),
                               level_map_memory_[i].size(), LevelMapCap);
    }

    // Construct PriceLevels — IntrusiveList sentinel needs proper init
    // (sentinel_.prev = sentinel_.next = &sentinel_, not nullptr from zeroed
    // pages). Orders are trivially destructible; zeroed memory is sufficient.
    for (std::size_t i = 0; i < MaxLevels; ++i) {
      std::construct_at(&level_slots_[i]);
    }

    // Free stacks are self-initializing (FixedIndexFreeStack constructor fills
    // [0, N)).
  }

  // Non-copyable, non-movable (contains IntrusiveLists and large arrays).
  OrderBook(const OrderBook &) = delete;
  OrderBook &operator=(const OrderBook &) = delete;
  OrderBook(OrderBook &&) = delete;
  OrderBook &operator=(OrderBook &&) = delete;

  ~OrderBook() noexcept {
    clear();
    // Explicitly destroy PriceLevels before MmapRegion unmaps memory.
    // After clear(), all IntrusiveLists are empty, so destructors are cheap
    // (no-op clear). But skipping destructors for non-trivially-destructible
    // types is UB.
    if (level_slots_) {
      for (std::size_t i = 0; i < MaxLevels; ++i) {
        std::destroy_at(&level_slots_[i]);
      }
    }
    // Orders are trivially destructible — no explicit destruction needed.
    // MmapRegion destructors handle munmap.
  }

  // ---------------------------------------------------------------------------
  // Modifiers
  // ---------------------------------------------------------------------------

  /// Add a resting limit order to the book.
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
    ++level->order_count;

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
    --level->order_count;

    // If level is now empty, remove it from the ladder and free it.
    if (level->order_count == 0) {
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
      --level->order_count;

      if (level->order_count == 0) {
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

    clear_side(bids_, 0);
    clear_side(asks_, 1);
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
    // const method → level_map_ const is HashMap → find() const returns Value*
    // Value = PriceLevel*, const so Value* = const(PriceLevel*)* = PriceLevel*
    // const*
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
    return (*pp)->order_count;
  }

  /// @return true if an order with the given ID exists in the book.
  [[nodiscard]] bool has_order(OrderId id) const noexcept {
    return order_map_.find(id) != nullptr;
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
    --level->order_count;

    order_map_.erase(order.order_id);
    free_order(&order);

    if (level->order_count == 0) {
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
  /// Convert Side enum to array index with bounds check.
  /// Side is uint8_t with values 0/1 — this guards against invalid casts.
  static constexpr int side_index(Side s) noexcept {
    const int idx = static_cast<int>(s);
    assert(idx >= 0 && idx <= 1 && "Invalid Side enum value");
    return idx;
  }

  // ---------------------------------------------------------------------------
  // Pool management (FixedIndexFreeStack + MmapRegion-backed slots)
  // ---------------------------------------------------------------------------
  //
  // Each pool is an MmapRegion-backed array of pre-allocated slots plus an
  // FixedIndexFreeStack managing free slot indices.
  //
  // Allocate: pop index from free stack, return &slots_[index].
  // Deallocate: compute index from pointer arithmetic, push to free stack.
  //
  // This is the simplest possible single-threaded pool — no lock-free
  // overhead, no alignment constraints beyond T's natural alignment.

  [[nodiscard]] Order *alloc_order() noexcept {
    auto idx = order_free_.pop();
    if (!idx) [[unlikely]] {
      return nullptr;
    }
    return &order_slots_[*idx];
  }

  void free_order(Order *order) noexcept {
    assert(order >= &order_slots_[0] && order < &order_slots_[0] + MaxOrders);
    auto idx = static_cast<std::uint32_t>(order - &order_slots_[0]);
    // Reset the order's hook state so it's cleanly reusable.
    order->prev = nullptr;
    order->next = nullptr;
    order->level = nullptr;
    order_free_.push(idx);
  }

  [[nodiscard]] PriceLevel *alloc_level() noexcept {
    auto idx = level_free_.pop();
    if (!idx) [[unlikely]] {
      return nullptr;
    }
    return &level_slots_[*idx];
  }

  void free_level(PriceLevel *level) noexcept {
    assert(level >= &level_slots_[0] && level < &level_slots_[0] + MaxLevels);
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
    level->order_count = 0;
    level->side = side;
    // level->orders is already default-constructed (empty IntrusiveList).

    // Insert into the level map.
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
    assert(level->order_count == 0);
    assert(level->orders.empty());

    const int si = side_index(level->side);
    auto &ladder = (level->side == Side::kBid) ? bids_ : asks_;

    ladder.erase(*level);
    level_map_[si].erase(level->price);
    free_level(level);
  }

  // ---------------------------------------------------------------------------
  // Data members
  // ---------------------------------------------------------------------------

  // Pools — MmapRegion-backed storage with FixedIndexFreeStack for slot
  // management.
  mk::sys::memory::MmapRegion order_memory_;
  Order *order_slots_{nullptr};
  mk::ds::FixedIndexFreeStack<MaxOrders> order_free_;

  mk::sys::memory::MmapRegion level_memory_;
  PriceLevel *level_slots_{nullptr};
  mk::ds::FixedIndexFreeStack<MaxLevels> level_free_;

  // Sorted price ladders.
  mk::ds::IntrusiveList<PriceLevel> bids_; ///< Descending (best = front).
  mk::ds::IntrusiveList<PriceLevel> asks_; ///< Ascending (best = front).

  // Lookup maps — HashMap with MmapRegion-backed buffers.
  // Unlike FixedHashMap (inline std::array), HashMap stores only a pointer
  // to externally-owned memory. This keeps OrderBook's sizeof small (~272KB
  // with default params instead of ~2.15MB) and gives explicit control over
  // page size, NUMA placement, and prefaulting for each map independently.
  mk::sys::memory::MmapRegion order_map_memory_;
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
  mk::sys::memory::MmapRegion level_map_memory_[2];
  mk::ds::HashMap<Price, PriceLevel *> level_map_[2];
};

} // namespace mk::algo
