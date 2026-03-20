/**
 * @file order_book_test.cpp
 * @brief Tests for mk::algo::OrderBook — limit order book data structure.
 */

#include "algo/order_book.hpp"

#include <vector>

#include <gtest/gtest.h>

namespace {

using mk::algo::OrderBook;
using mk::algo::OrderId;
using mk::algo::Price;
using mk::algo::Qty;
using mk::algo::Side;

// Use small capacities for tests to keep memory reasonable and to test
// pool exhaustion without allocating 64K orders.
// OrderMapCap must be larger than MaxOrders because HashMap enforces
// a 70% load factor. With MaxOrders=64, we need OrderMapCap >= 128.
using TestBook = OrderBook</*MaxOrders=*/64, /*MaxLevels=*/16,
                           /*OrderMapCap=*/128, /*LevelMapCap=*/32>;

// =============================================================================
// 1. EmptyBook
// =============================================================================

TEST(OrderBookTest, EmptyBook) {
  const TestBook book;
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.spread(), std::nullopt);
  EXPECT_EQ(book.book_depth(Side::kBid), 0U);
  EXPECT_EQ(book.book_depth(Side::kAsk), 0U);
  EXPECT_EQ(book.total_orders(), 0U);
}

// =============================================================================
// 2. AddSingleBid
// =============================================================================

TEST(OrderBookTest, AddSingleBid) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 10));

  EXPECT_EQ(book.best_bid(), 100);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.volume_at_level(Side::kBid, 100), 10U);
  EXPECT_EQ(book.order_count_at_level(Side::kBid, 100), 1U);
  EXPECT_EQ(book.book_depth(Side::kBid), 1U);
  EXPECT_EQ(book.total_orders(), 1U);
}

// =============================================================================
// 3. AddSingleAsk
// =============================================================================

TEST(OrderBookTest, AddSingleAsk) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kAsk, 105, 20));

  EXPECT_EQ(book.best_ask(), 105);
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.volume_at_level(Side::kAsk, 105), 20U);
}

// =============================================================================
// 4. BidAskSpread
// =============================================================================

TEST(OrderBookTest, BidAskSpread) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book.add_order(2, Side::kAsk, 102, 10));

  EXPECT_EQ(book.spread(), 2);
}

// =============================================================================
// 5. FIFOOrderAtSamePrice
// =============================================================================

TEST(OrderBookTest, FIFOOrderAtSamePrice) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book.add_order(2, Side::kBid, 100, 20));
  ASSERT_TRUE(book.add_order(3, Side::kBid, 100, 30));

  EXPECT_EQ(book.volume_at_level(Side::kBid, 100), 60U);
  EXPECT_EQ(book.order_count_at_level(Side::kBid, 100), 3U);
  EXPECT_EQ(book.book_depth(Side::kBid), 1U); // all at same price

  // Verify FIFO: cancel first order, second becomes front.
  ASSERT_TRUE(book.cancel_order(1));
  EXPECT_EQ(book.volume_at_level(Side::kBid, 100), 50U);
  EXPECT_EQ(book.order_count_at_level(Side::kBid, 100), 2U);
}

// =============================================================================
// 6. MultiplePriceLevels
// =============================================================================

TEST(OrderBookTest, MultiplePriceLevels) {
  TestBook book;
  // Bids: best = highest price.
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book.add_order(2, Side::kBid, 102, 10));
  ASSERT_TRUE(book.add_order(3, Side::kBid, 101, 10));

  EXPECT_EQ(book.best_bid(), 102);
  EXPECT_EQ(book.book_depth(Side::kBid), 3U);

  // Asks: best = lowest price.
  ASSERT_TRUE(book.add_order(4, Side::kAsk, 110, 10));
  ASSERT_TRUE(book.add_order(5, Side::kAsk, 105, 10));
  ASSERT_TRUE(book.add_order(6, Side::kAsk, 107, 10));

  EXPECT_EQ(book.best_ask(), 105);
  EXPECT_EQ(book.book_depth(Side::kAsk), 3U);
}

// =============================================================================
// 7. CancelOrder
// =============================================================================

TEST(OrderBookTest, CancelOrder) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book.cancel_order(1));

  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.total_orders(), 0U);

  // Cancel non-existent order.
  EXPECT_FALSE(book.cancel_order(999));
}

// =============================================================================
// 8. CancelMiddleOrder
// =============================================================================

TEST(OrderBookTest, CancelMiddleOrder) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book.add_order(2, Side::kBid, 100, 20));
  ASSERT_TRUE(book.add_order(3, Side::kBid, 100, 30));

  ASSERT_TRUE(book.cancel_order(2));

  EXPECT_EQ(book.volume_at_level(Side::kBid, 100), 40U);
  EXPECT_EQ(book.order_count_at_level(Side::kBid, 100), 2U);
  EXPECT_EQ(book.total_orders(), 2U);
}

// =============================================================================
// 9. CancelLastOrderAtLevel
// =============================================================================

TEST(OrderBookTest, CancelLastOrderAtLevel) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book.add_order(2, Side::kBid, 102, 20));

  // Cancel the only order at price 100. Level should be removed.
  ASSERT_TRUE(book.cancel_order(1));

  EXPECT_EQ(book.book_depth(Side::kBid), 1U);
  EXPECT_EQ(book.best_bid(), 102);
  EXPECT_EQ(book.volume_at_level(Side::kBid, 100), 0U);
}

// =============================================================================
// 10. ModifyReduceQty
// =============================================================================

TEST(OrderBookTest, ModifyReduceQty) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 50));

  ASSERT_TRUE(book.modify_order(1, 30));

  EXPECT_EQ(book.volume_at_level(Side::kBid, 100), 30U);
  EXPECT_EQ(book.total_orders(), 1U);
}

// =============================================================================
// 11. ModifyToZeroCancels
// =============================================================================

TEST(OrderBookTest, ModifyToZeroCancels) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 50));

  ASSERT_TRUE(book.modify_order(1, 0));

  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.total_orders(), 0U);
}

// =============================================================================
// 12. ModifyRejectsIncrease
// =============================================================================

TEST(OrderBookTest, ModifyRejectsIncrease) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 50));

  // Increasing qty loses time priority — not allowed.
  EXPECT_FALSE(book.modify_order(1, 60));
  // Same qty also rejected.
  EXPECT_FALSE(book.modify_order(1, 50));

  // Qty unchanged.
  EXPECT_EQ(book.volume_at_level(Side::kBid, 100), 50U);
}

// =============================================================================
// 13. BookDepth
// =============================================================================

TEST(OrderBookTest, BookDepth) {
  TestBook book;
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(book.add_order(i + 1, Side::kBid, 100 - i, 10));
  }
  EXPECT_EQ(book.book_depth(Side::kBid), 5U);
}

// =============================================================================
// 14. PoolExhaustion
// =============================================================================

TEST(OrderBookTest, PoolExhaustion) {
  // TestBook has MaxOrders=64. Fill it up, then verify add fails.
  TestBook book;

  for (OrderId i = 1; i <= 64; ++i) {
    ASSERT_TRUE(book.add_order(i, Side::kBid, 100, 1));
  }

  // 65th order should fail — pool exhausted.
  EXPECT_FALSE(book.add_order(65, Side::kBid, 100, 1));

  // Cancel one, then adding should succeed again.
  ASSERT_TRUE(book.cancel_order(1));
  EXPECT_TRUE(book.add_order(65, Side::kBid, 100, 1));
}

// =============================================================================
// 15. DuplicateOrderIdRejected
// =============================================================================

TEST(OrderBookTest, DuplicateOrderIdRejected) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 10));

  // Same ID, different price/qty — should still be rejected.
  EXPECT_FALSE(book.add_order(1, Side::kAsk, 200, 20));

  EXPECT_EQ(book.total_orders(), 1U);
}

// =============================================================================
// 16. ZeroQtyRejected
// =============================================================================

TEST(OrderBookTest, ZeroQtyRejected) {
  TestBook book;
  EXPECT_FALSE(book.add_order(1, Side::kBid, 100, 0));
  EXPECT_EQ(book.total_orders(), 0U);
}

// =============================================================================
// 17. ClearResetsBook
// =============================================================================

TEST(OrderBookTest, ClearResetsBook) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book.add_order(2, Side::kAsk, 105, 20));

  book.clear();

  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.total_orders(), 0U);
  EXPECT_EQ(book.book_depth(Side::kBid), 0U);
  EXPECT_EQ(book.book_depth(Side::kAsk), 0U);

  // Should be reusable after clear.
  ASSERT_TRUE(book.add_order(3, Side::kBid, 200, 50));
  EXPECT_EQ(book.best_bid(), 200);
}

// =============================================================================
// 17b. ClearIsIdempotent — double-clear must not corrupt state
// =============================================================================

TEST(OrderBookTest, ClearIsIdempotent) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book.add_order(2, Side::kAsk, 105, 20));
  ASSERT_TRUE(book.add_order(3, Side::kBid, 100, 30));

  book.clear();
  book.clear(); // Second clear must be a no-op, not a double-free.

  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.total_orders(), 0U);

  // Book must be fully reusable after double-clear.
  ASSERT_TRUE(book.add_order(4, Side::kBid, 200, 50));
  EXPECT_EQ(book.best_bid(), 200);
  EXPECT_EQ(book.total_orders(), 1U);
}

// =============================================================================
// 18. BidLadderSortedDescending
// =============================================================================

TEST(OrderBookTest, BidLadderSortedDescending) {
  TestBook book;
  // Insert in random order.
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 1));
  ASSERT_TRUE(book.add_order(2, Side::kBid, 103, 1));
  ASSERT_TRUE(book.add_order(3, Side::kBid, 101, 1));
  ASSERT_TRUE(book.add_order(4, Side::kBid, 104, 1));
  ASSERT_TRUE(book.add_order(5, Side::kBid, 102, 1));

  // Walk ladder: should be 104, 103, 102, 101, 100.
  std::vector<Price> prices;
  for (const auto &level : book.bids()) {
    prices.push_back(level.price);
  }

  std::vector<Price> const expected = {104, 103, 102, 101, 100};
  EXPECT_EQ(prices, expected);
}

// =============================================================================
// 19. AskLadderSortedAscending
// =============================================================================

TEST(OrderBookTest, AskLadderSortedAscending) {
  TestBook book;
  // Insert in random order.
  ASSERT_TRUE(book.add_order(1, Side::kAsk, 105, 1));
  ASSERT_TRUE(book.add_order(2, Side::kAsk, 102, 1));
  ASSERT_TRUE(book.add_order(3, Side::kAsk, 104, 1));
  ASSERT_TRUE(book.add_order(4, Side::kAsk, 101, 1));
  ASSERT_TRUE(book.add_order(5, Side::kAsk, 103, 1));

  // Walk ladder: should be 101, 102, 103, 104, 105.
  std::vector<Price> prices;
  for (const auto &level : book.asks()) {
    prices.push_back(level.price);
  }

  std::vector<Price> const expected = {101, 102, 103, 104, 105};
  EXPECT_EQ(prices, expected);
}

// =============================================================================
// 20. LevelPoolExhaustion
// =============================================================================

TEST(OrderBookTest, LevelPoolExhaustion) {
  // TestBook has MaxLevels=16. Each distinct price creates a new level.
  TestBook book;

  // Fill all 16 level slots with 16 distinct bid prices.
  for (OrderId i = 1; i <= 16; ++i) {
    ASSERT_TRUE(book.add_order(i, Side::kBid, static_cast<Price>(100 + i), 1));
  }
  ASSERT_EQ(book.book_depth(Side::kBid), 16U);
  ASSERT_EQ(book.free_level_count(), 0U);

  // 17th distinct price should fail — level pool exhausted.
  EXPECT_FALSE(book.add_order(17, Side::kBid, 200, 1));
  EXPECT_EQ(book.total_orders(), 16U);

  // Adding at an existing price should still succeed (no new level needed).
  EXPECT_TRUE(book.add_order(17, Side::kBid, 101, 1));

  // Cancel an order that removes a level, then adding a new price succeeds.
  ASSERT_TRUE(book.cancel_order(17)); // Remove the extra order at 101.
  ASSERT_TRUE(book.cancel_order(1)); // Remove sole order at 101 -> frees level.
  EXPECT_EQ(book.free_level_count(), 1U);

  EXPECT_TRUE(book.add_order(18, Side::kBid, 200, 1));
  EXPECT_EQ(book.best_bid(), 200);
}

// =============================================================================
// 21. MapExhaustion
// =============================================================================

TEST(OrderBookTest, MapExhaustion) {
  // Create a book where the order map is the bottleneck, not the order pool.
  // OrderMapCap=32 -> kMaxLoad = 32 * 7 / 10 = 22. MaxOrders=32 > 22.
  // LevelMapCap must also be large enough to not be the bottleneck.
  using MapLimitedBook = OrderBook</*MaxOrders=*/32, /*MaxLevels=*/32,
                                   /*OrderMapCap=*/32, /*LevelMapCap=*/64>;
  MapLimitedBook book;

  // Insert until the map rejects (should happen around 22 inserts).
  OrderId id = 1;
  while (book.add_order(id, Side::kBid, 100, 1)) {
    ++id;
  }

  // The map should have capped out before the order pool (32).
  EXPECT_LT(book.total_orders(), 32U);
  // kMaxLoad = 32 * 7 / 10 = 22.
  EXPECT_EQ(book.total_orders(), 22U);

  // Note: after cancel, the map leaves a tombstone. FixedHashMap counts
  // (size + tombstones) against kMaxLoad, so a cancel-then-reinsert may
  // still fail unless the probe happens to land on the tombstone slot.
  // This is by-design: tombstone-heavy tables degrade and need clear().
}

// =============================================================================
// 22. LevelMapExhaustion
// =============================================================================

TEST(OrderBookTest, LevelMapExhaustion) {
  // MaxLevels=32 (pool), LevelMapCap=32 -> kMaxLoad = 32 * 7 / 10 = 22.
  // The level map becomes the bottleneck before the level pool.
  using LevelMapLimitedBook =
      OrderBook</*MaxOrders=*/64, /*MaxLevels=*/32,
                /*OrderMapCap=*/128, /*LevelMapCap=*/32>;
  LevelMapLimitedBook book;

  // Add orders at 22 distinct prices to fill the level map to its load limit.
  for (OrderId i = 1; i <= 22; ++i) {
    ASSERT_TRUE(book.add_order(i, Side::kBid, static_cast<Price>(100 + i), 1));
  }
  ASSERT_EQ(book.free_level_count(), 32U - 22U);

  // 23rd distinct price should fail — level map exhausted.
  EXPECT_FALSE(book.add_order(23, Side::kBid, 200, 1));

  // Adding to an existing price level should still succeed (no new level).
  EXPECT_TRUE(book.add_order(23, Side::kBid, 101, 1));
}

// =============================================================================
// 23. AddAfterCancelSameId
// =============================================================================

TEST(OrderBookTest, AddAfterCancelSameId) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 10));
  ASSERT_EQ(book.total_orders(), 1U);

  // Cancel the order — leaves a tombstone in order_map_.
  ASSERT_TRUE(book.cancel_order(1));
  ASSERT_EQ(book.total_orders(), 0U);

  // Re-add with the same ID. insert() should reuse the tombstone slot.
  EXPECT_TRUE(book.add_order(1, Side::kAsk, 200, 20));
  ASSERT_EQ(book.total_orders(), 1U);
  EXPECT_EQ(book.best_ask(), 200);
  EXPECT_EQ(book.volume_at_level(Side::kAsk, 200), 20U);
}

// =============================================================================
// 24. ModifyMissingOrder
// =============================================================================

TEST(OrderBookTest, ModifyMissingOrder) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 50));

  // Modify non-existent order — should return false, book unchanged.
  EXPECT_FALSE(book.modify_order(999, 25));

  EXPECT_EQ(book.total_orders(), 1U);
  EXPECT_EQ(book.volume_at_level(Side::kBid, 100), 50U);
}

// =============================================================================
// 25. RemoveFilledOrderLevelSurvives
// =============================================================================

TEST(OrderBookTest, RemoveFilledOrderLevelSurvives) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kAsk, 100, 10));
  ASSERT_TRUE(book.add_order(2, Side::kAsk, 100, 20));
  ASSERT_TRUE(book.add_order(3, Side::kAsk, 100, 30));

  // remove_filled_order on the front order (FIFO head).
  // The level should survive because orders 2 and 3 remain.
  auto &ask_ladder = book.asks();
  ASSERT_FALSE(ask_ladder.empty());
  auto &level = ask_ladder.front();
  auto &front_order = level.orders.front();
  ASSERT_EQ(front_order.order_id, 1U);

  const bool level_destroyed = book.remove_filled_order(front_order);
  EXPECT_FALSE(level_destroyed);

  // Verify book state.
  EXPECT_EQ(book.total_orders(), 2U);
  EXPECT_EQ(book.book_depth(Side::kAsk), 1U);
  EXPECT_EQ(book.volume_at_level(Side::kAsk, 100), 50U);
  EXPECT_EQ(book.order_count_at_level(Side::kAsk, 100), 2U);

  // Verify remaining FIFO order: 2 then 3.
  auto &surviving_level = ask_ladder.front();
  auto it = surviving_level.orders.begin();
  EXPECT_EQ(it->order_id, 2U);
  ++it;
  EXPECT_EQ(it->order_id, 3U);
  ++it;
  EXPECT_EQ(it, surviving_level.orders.end());
}

// =============================================================================
// 26. RemoveFilledOrderLevelDestroyed
// =============================================================================

TEST(OrderBookTest, RemoveFilledOrderLevelDestroyed) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book.add_order(2, Side::kBid, 102, 20));

  // Remove the sole order at price 102 — level should be destroyed.
  auto &bid_ladder = book.bids();
  ASSERT_FALSE(bid_ladder.empty());
  // Best bid = 102, which is front.
  auto &best_level = bid_ladder.front();
  ASSERT_EQ(best_level.price, 102);
  auto &only_order = best_level.orders.front();
  ASSERT_EQ(only_order.order_id, 2U);

  const bool level_destroyed = book.remove_filled_order(only_order);
  EXPECT_TRUE(level_destroyed);

  // Level at 102 is gone, best bid is now 100.
  EXPECT_EQ(book.total_orders(), 1U);
  EXPECT_EQ(book.book_depth(Side::kBid), 1U);
  EXPECT_EQ(book.best_bid(), 100);
  EXPECT_EQ(book.volume_at_level(Side::kBid, 102), 0U);
}

// =============================================================================
// 27. RestOrder
// =============================================================================

TEST(OrderBookTest, RestOrder) {
  TestBook book;
  // rest_order is a semantic alias for add_order — verify it works.
  ASSERT_TRUE(book.rest_order(1, Side::kAsk, 105, 50));

  EXPECT_EQ(book.best_ask(), 105);
  EXPECT_EQ(book.volume_at_level(Side::kAsk, 105), 50U);
  EXPECT_EQ(book.total_orders(), 1U);

  // Duplicate ID rejected — same validation as add_order.
  EXPECT_FALSE(book.rest_order(1, Side::kBid, 100, 10));
  EXPECT_EQ(book.total_orders(), 1U);
}

// =============================================================================
// 28. FIFOOrderSequenceAfterCancel (strengthened FIFO test)
// =============================================================================

TEST(OrderBookTest, FIFOOrderSequenceAfterCancel) {
  TestBook book;
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book.add_order(2, Side::kBid, 100, 20));
  ASSERT_TRUE(book.add_order(3, Side::kBid, 100, 30));
  ASSERT_TRUE(book.add_order(4, Side::kBid, 100, 40));

  // Cancel middle order (2). Remaining sequence: 1, 3, 4.
  ASSERT_TRUE(book.cancel_order(2));

  auto &bid_ladder = book.bids();
  ASSERT_FALSE(bid_ladder.empty());
  auto &level = bid_ladder.front();

  std::vector<OrderId> remaining_ids;
  for (const auto &order : level.orders) {
    remaining_ids.push_back(order.order_id);
  }
  std::vector<OrderId> expected = {1, 3, 4};
  EXPECT_EQ(remaining_ids, expected);

  // Cancel front order (1). Remaining sequence: 3, 4.
  ASSERT_TRUE(book.cancel_order(1));
  remaining_ids.clear();
  // Level reference is still valid (not destroyed, orders remain).
  for (const auto &order : level.orders) {
    remaining_ids.push_back(order.order_id);
  }
  expected = {3, 4};
  EXPECT_EQ(remaining_ids, expected);

  // Cancel back order (4). Remaining sequence: 3.
  ASSERT_TRUE(book.cancel_order(4));
  remaining_ids.clear();
  for (const auto &order : level.orders) {
    remaining_ids.push_back(order.order_id);
  }
  expected = {3};
  EXPECT_EQ(remaining_ids, expected);
}

// =============================================================================
// 29. SizeofIsSmall — verify HashMap refactor reduced inline size
// =============================================================================

TEST(OrderBookSizeTest, SizeofIsSmall) {
  // After FixedHashMap → HashMap refactor, OrderBook's inline size should be
  // dramatically smaller. With default params, the inline std::array storage
  // for FixedHashMap was ~1.9MB. HashMap uses MmapRegion-backed external
  // buffers, leaving only FixedIndexFreeStacks (~272KB) as the main inline
  // data.
  using DefaultBook = OrderBook<>;
  EXPECT_LT(sizeof(DefaultBook), 512 * 1024); // < 512KB (conservative)

  // Test-sized book should be tiny.
  EXPECT_LT(sizeof(TestBook), 4096); // < 4KB
}

} // namespace
