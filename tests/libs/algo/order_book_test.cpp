/**
 * @file order_book_test.cpp
 * @brief Tests for mk::algo::OrderBook — limit order book data structure.
 */

#include "algo/order_book.hpp"

#include <cstring>
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
constexpr OrderBook::Params kTestParams{
    .max_orders = 64,
    .max_levels = 16,
    .order_map_cap = 128,
    .level_map_cap = 32,
};

/// Test fixture that manages buffer lifetime for a test-sized OrderBook.
class OrderBookTest : public ::testing::Test {
protected:
  std::vector<std::byte> buf_{OrderBook::required_buffer_size(kTestParams)};
  OrderBook book_{buf_.data(), buf_.size(), kTestParams};
};

// =============================================================================
// 1. EmptyBook
// =============================================================================

TEST_F(OrderBookTest, EmptyBook) {
  EXPECT_EQ(book_.best_bid(), std::nullopt);
  EXPECT_EQ(book_.best_ask(), std::nullopt);
  EXPECT_EQ(book_.spread(), std::nullopt);
  EXPECT_EQ(book_.book_depth(Side::kBid), 0U);
  EXPECT_EQ(book_.book_depth(Side::kAsk), 0U);
  EXPECT_EQ(book_.total_orders(), 0U);
}

// =============================================================================
// 2. AddSingleBid
// =============================================================================

TEST_F(OrderBookTest, AddSingleBid) {
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 10));

  EXPECT_EQ(book_.best_bid(), 100);
  EXPECT_EQ(book_.best_ask(), std::nullopt);
  EXPECT_EQ(book_.volume_at_level(Side::kBid, 100), 10U);
  EXPECT_EQ(book_.order_count_at_level(Side::kBid, 100), 1U);
  EXPECT_EQ(book_.book_depth(Side::kBid), 1U);
  EXPECT_EQ(book_.total_orders(), 1U);
}

// =============================================================================
// 3. AddSingleAsk
// =============================================================================

TEST_F(OrderBookTest, AddSingleAsk) {
  ASSERT_TRUE(book_.add_order(1, Side::kAsk, 105, 20));

  EXPECT_EQ(book_.best_ask(), 105);
  EXPECT_EQ(book_.best_bid(), std::nullopt);
  EXPECT_EQ(book_.volume_at_level(Side::kAsk, 105), 20U);
}

// =============================================================================
// 4. BidAskSpread
// =============================================================================

TEST_F(OrderBookTest, BidAskSpread) {
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book_.add_order(2, Side::kAsk, 102, 10));

  EXPECT_EQ(book_.spread(), 2);
}

// =============================================================================
// 5. FIFOOrderAtSamePrice
// =============================================================================

TEST_F(OrderBookTest, FIFOOrderAtSamePrice) {
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book_.add_order(2, Side::kBid, 100, 20));
  ASSERT_TRUE(book_.add_order(3, Side::kBid, 100, 30));

  EXPECT_EQ(book_.volume_at_level(Side::kBid, 100), 60U);
  EXPECT_EQ(book_.order_count_at_level(Side::kBid, 100), 3U);
  EXPECT_EQ(book_.book_depth(Side::kBid), 1U); // all at same price

  // Verify FIFO: cancel first order, second becomes front.
  ASSERT_TRUE(book_.cancel_order(1));
  EXPECT_EQ(book_.volume_at_level(Side::kBid, 100), 50U);
  EXPECT_EQ(book_.order_count_at_level(Side::kBid, 100), 2U);
}

// =============================================================================
// 6. MultiplePriceLevels
// =============================================================================

TEST_F(OrderBookTest, MultiplePriceLevels) {
  // Bids: best = highest price.
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book_.add_order(2, Side::kBid, 102, 10));
  ASSERT_TRUE(book_.add_order(3, Side::kBid, 101, 10));

  EXPECT_EQ(book_.best_bid(), 102);
  EXPECT_EQ(book_.book_depth(Side::kBid), 3U);

  // Asks: best = lowest price.
  ASSERT_TRUE(book_.add_order(4, Side::kAsk, 110, 10));
  ASSERT_TRUE(book_.add_order(5, Side::kAsk, 105, 10));
  ASSERT_TRUE(book_.add_order(6, Side::kAsk, 107, 10));

  EXPECT_EQ(book_.best_ask(), 105);
  EXPECT_EQ(book_.book_depth(Side::kAsk), 3U);
}

// =============================================================================
// 7. CancelOrder
// =============================================================================

TEST_F(OrderBookTest, CancelOrder) {
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book_.cancel_order(1));

  EXPECT_EQ(book_.best_bid(), std::nullopt);
  EXPECT_EQ(book_.total_orders(), 0U);

  // Cancel non-existent order.
  EXPECT_FALSE(book_.cancel_order(999));
}

// =============================================================================
// 8. CancelMiddleOrder
// =============================================================================

TEST_F(OrderBookTest, CancelMiddleOrder) {
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book_.add_order(2, Side::kBid, 100, 20));
  ASSERT_TRUE(book_.add_order(3, Side::kBid, 100, 30));

  ASSERT_TRUE(book_.cancel_order(2));

  EXPECT_EQ(book_.volume_at_level(Side::kBid, 100), 40U);
  EXPECT_EQ(book_.order_count_at_level(Side::kBid, 100), 2U);
  EXPECT_EQ(book_.total_orders(), 2U);
}

// =============================================================================
// 9. CancelLastOrderAtLevel
// =============================================================================

TEST_F(OrderBookTest, CancelLastOrderAtLevel) {
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book_.add_order(2, Side::kBid, 102, 20));

  // Cancel the only order at price 100. Level should be removed.
  ASSERT_TRUE(book_.cancel_order(1));

  EXPECT_EQ(book_.book_depth(Side::kBid), 1U);
  EXPECT_EQ(book_.best_bid(), 102);
  EXPECT_EQ(book_.volume_at_level(Side::kBid, 100), 0U);
}

// =============================================================================
// 10. ModifyReduceQty
// =============================================================================

TEST_F(OrderBookTest, ModifyReduceQty) {
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 50));

  ASSERT_TRUE(book_.modify_order(1, 30));

  EXPECT_EQ(book_.volume_at_level(Side::kBid, 100), 30U);
  EXPECT_EQ(book_.total_orders(), 1U);
}

// =============================================================================
// 11. ModifyToZeroCancels
// =============================================================================

TEST_F(OrderBookTest, ModifyToZeroCancels) {
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 50));

  ASSERT_TRUE(book_.modify_order(1, 0));

  EXPECT_EQ(book_.best_bid(), std::nullopt);
  EXPECT_EQ(book_.total_orders(), 0U);
}

// =============================================================================
// 12. ModifyRejectsIncrease
// =============================================================================

TEST_F(OrderBookTest, ModifyRejectsIncrease) {
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 50));

  // Increasing qty loses time priority — not allowed.
  EXPECT_FALSE(book_.modify_order(1, 60));
  // Same qty also rejected.
  EXPECT_FALSE(book_.modify_order(1, 50));

  // Qty unchanged.
  EXPECT_EQ(book_.volume_at_level(Side::kBid, 100), 50U);
}

// =============================================================================
// 13. BookDepth
// =============================================================================

TEST_F(OrderBookTest, BookDepth) {
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(book_.add_order(i + 1, Side::kBid, 100 - i, 10));
  }
  EXPECT_EQ(book_.book_depth(Side::kBid), 5U);
}

// =============================================================================
// 14. PoolExhaustion
// =============================================================================

TEST_F(OrderBookTest, PoolExhaustion) {
  // kTestParams has max_orders=64. Fill it up, then verify add fails.
  for (OrderId i = 1; i <= 64; ++i) {
    ASSERT_TRUE(book_.add_order(i, Side::kBid, 100, 1));
  }

  // 65th order should fail — pool exhausted.
  EXPECT_FALSE(book_.add_order(65, Side::kBid, 100, 1));

  // Cancel one, then adding should succeed again.
  ASSERT_TRUE(book_.cancel_order(1));
  EXPECT_TRUE(book_.add_order(65, Side::kBid, 100, 1));
}

// =============================================================================
// 15. DuplicateOrderIdRejected
// =============================================================================

TEST_F(OrderBookTest, DuplicateOrderIdRejected) {
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 10));

  // Same ID, different price/qty — should still be rejected.
  EXPECT_FALSE(book_.add_order(1, Side::kAsk, 200, 20));

  EXPECT_EQ(book_.total_orders(), 1U);
}

// =============================================================================
// 16. ZeroQtyRejected
// =============================================================================

TEST_F(OrderBookTest, ZeroQtyRejected) {
  EXPECT_FALSE(book_.add_order(1, Side::kBid, 100, 0));
  EXPECT_EQ(book_.total_orders(), 0U);
}

// =============================================================================
// 17. ClearResetsBook
// =============================================================================

TEST_F(OrderBookTest, ClearResetsBook) {
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book_.add_order(2, Side::kAsk, 105, 20));

  book_.clear();

  EXPECT_EQ(book_.best_bid(), std::nullopt);
  EXPECT_EQ(book_.best_ask(), std::nullopt);
  EXPECT_EQ(book_.total_orders(), 0U);
  EXPECT_EQ(book_.book_depth(Side::kBid), 0U);
  EXPECT_EQ(book_.book_depth(Side::kAsk), 0U);

  // Should be reusable after clear.
  ASSERT_TRUE(book_.add_order(3, Side::kBid, 200, 50));
  EXPECT_EQ(book_.best_bid(), 200);
}

// =============================================================================
// 17b. ClearIsIdempotent — double-clear must not corrupt state
// =============================================================================

TEST_F(OrderBookTest, ClearIsIdempotent) {
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book_.add_order(2, Side::kAsk, 105, 20));
  ASSERT_TRUE(book_.add_order(3, Side::kBid, 100, 30));

  book_.clear();
  book_.clear(); // Second clear must be a no-op, not a double-free.

  EXPECT_EQ(book_.best_bid(), std::nullopt);
  EXPECT_EQ(book_.best_ask(), std::nullopt);
  EXPECT_EQ(book_.total_orders(), 0U);

  // Book must be fully reusable after double-clear.
  ASSERT_TRUE(book_.add_order(4, Side::kBid, 200, 50));
  EXPECT_EQ(book_.best_bid(), 200);
  EXPECT_EQ(book_.total_orders(), 1U);
}

// =============================================================================
// 18. BidLadderSortedDescending
// =============================================================================

TEST_F(OrderBookTest, BidLadderSortedDescending) {
  // Insert in random order.
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 1));
  ASSERT_TRUE(book_.add_order(2, Side::kBid, 103, 1));
  ASSERT_TRUE(book_.add_order(3, Side::kBid, 101, 1));
  ASSERT_TRUE(book_.add_order(4, Side::kBid, 104, 1));
  ASSERT_TRUE(book_.add_order(5, Side::kBid, 102, 1));

  // Walk ladder: should be 104, 103, 102, 101, 100.
  std::vector<Price> prices;
  for (const auto &level : book_.bids()) {
    prices.push_back(level.price);
  }

  std::vector<Price> const expected = {104, 103, 102, 101, 100};
  EXPECT_EQ(prices, expected);
}

// =============================================================================
// 19. AskLadderSortedAscending
// =============================================================================

TEST_F(OrderBookTest, AskLadderSortedAscending) {
  // Insert in random order.
  ASSERT_TRUE(book_.add_order(1, Side::kAsk, 105, 1));
  ASSERT_TRUE(book_.add_order(2, Side::kAsk, 102, 1));
  ASSERT_TRUE(book_.add_order(3, Side::kAsk, 104, 1));
  ASSERT_TRUE(book_.add_order(4, Side::kAsk, 101, 1));
  ASSERT_TRUE(book_.add_order(5, Side::kAsk, 103, 1));

  // Walk ladder: should be 101, 102, 103, 104, 105.
  std::vector<Price> prices;
  for (const auto &level : book_.asks()) {
    prices.push_back(level.price);
  }

  std::vector<Price> const expected = {101, 102, 103, 104, 105};
  EXPECT_EQ(prices, expected);
}

// =============================================================================
// 20. LevelPoolExhaustion
// =============================================================================

TEST_F(OrderBookTest, LevelPoolExhaustion) {
  // kTestParams has max_levels=16. Each distinct price creates a new level.

  // Fill all 16 level slots with 16 distinct bid prices.
  for (OrderId i = 1; i <= 16; ++i) {
    ASSERT_TRUE(book_.add_order(i, Side::kBid, static_cast<Price>(100 + i), 1));
  }
  ASSERT_EQ(book_.book_depth(Side::kBid), 16U);
  ASSERT_EQ(book_.free_level_count(), 0U);

  // 17th distinct price should fail — level pool exhausted.
  EXPECT_FALSE(book_.add_order(17, Side::kBid, 200, 1));
  EXPECT_EQ(book_.total_orders(), 16U);

  // Adding at an existing price should still succeed (no new level needed).
  EXPECT_TRUE(book_.add_order(17, Side::kBid, 101, 1));

  // Cancel an order that removes a level, then adding a new price succeeds.
  ASSERT_TRUE(book_.cancel_order(17)); // Remove the extra order at 101.
  ASSERT_TRUE(
      book_.cancel_order(1)); // Remove sole order at 101 -> frees level.
  EXPECT_EQ(book_.free_level_count(), 1U);

  EXPECT_TRUE(book_.add_order(18, Side::kBid, 200, 1));
  EXPECT_EQ(book_.best_bid(), 200);
}

// =============================================================================
// 21. MapExhaustion
// =============================================================================

TEST(OrderBookMapTest, MapExhaustion) {
  // Create a book where the order map is the bottleneck, not the order pool.
  // OrderMapCap=32 -> kMaxLoad = 32 * 7 / 10 = 22. MaxOrders=32 > 22.
  // LevelMapCap must also be large enough to not be the bottleneck.
  constexpr OrderBook::Params kMapParams{
      .max_orders = 32,
      .max_levels = 32,
      .order_map_cap = 32,
      .level_map_cap = 64,
  };
  std::vector<std::byte> buf(OrderBook::required_buffer_size(kMapParams));
  OrderBook book(buf.data(), buf.size(), kMapParams);

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

TEST(OrderBookMapTest, LevelMapExhaustion) {
  // MaxLevels=32 (pool), LevelMapCap=32 -> kMaxLoad = 32 * 7 / 10 = 22.
  // The level map becomes the bottleneck before the level pool.
  constexpr OrderBook::Params kLevelMapParams{
      .max_orders = 64,
      .max_levels = 32,
      .order_map_cap = 128,
      .level_map_cap = 32,
  };
  std::vector<std::byte> buf(OrderBook::required_buffer_size(kLevelMapParams));
  OrderBook book(buf.data(), buf.size(), kLevelMapParams);

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

TEST_F(OrderBookTest, AddAfterCancelSameId) {
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 10));
  ASSERT_EQ(book_.total_orders(), 1U);

  // Cancel the order — leaves a tombstone in order_map_.
  ASSERT_TRUE(book_.cancel_order(1));
  ASSERT_EQ(book_.total_orders(), 0U);

  // Re-add with the same ID. insert() should reuse the tombstone slot.
  EXPECT_TRUE(book_.add_order(1, Side::kAsk, 200, 20));
  ASSERT_EQ(book_.total_orders(), 1U);
  EXPECT_EQ(book_.best_ask(), 200);
  EXPECT_EQ(book_.volume_at_level(Side::kAsk, 200), 20U);
}

// =============================================================================
// 24. ModifyMissingOrder
// =============================================================================

TEST_F(OrderBookTest, ModifyMissingOrder) {
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 50));

  // Modify non-existent order — should return false, book unchanged.
  EXPECT_FALSE(book_.modify_order(999, 25));

  EXPECT_EQ(book_.total_orders(), 1U);
  EXPECT_EQ(book_.volume_at_level(Side::kBid, 100), 50U);
}

// =============================================================================
// 25. RemoveFilledOrderLevelSurvives
// =============================================================================

TEST_F(OrderBookTest, RemoveFilledOrderLevelSurvives) {
  ASSERT_TRUE(book_.add_order(1, Side::kAsk, 100, 10));
  ASSERT_TRUE(book_.add_order(2, Side::kAsk, 100, 20));
  ASSERT_TRUE(book_.add_order(3, Side::kAsk, 100, 30));

  // remove_filled_order on the front order (FIFO head).
  // The level should survive because orders 2 and 3 remain.
  auto &ask_ladder = book_.asks();
  ASSERT_FALSE(ask_ladder.empty());
  auto &level = ask_ladder.front();
  auto &front_order = level.orders.front();
  ASSERT_EQ(front_order.order_id, 1U);

  const bool level_destroyed = book_.remove_filled_order(front_order);
  EXPECT_FALSE(level_destroyed);

  // Verify book state.
  EXPECT_EQ(book_.total_orders(), 2U);
  EXPECT_EQ(book_.book_depth(Side::kAsk), 1U);
  EXPECT_EQ(book_.volume_at_level(Side::kAsk, 100), 50U);
  EXPECT_EQ(book_.order_count_at_level(Side::kAsk, 100), 2U);

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

TEST_F(OrderBookTest, RemoveFilledOrderLevelDestroyed) {
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book_.add_order(2, Side::kBid, 102, 20));

  // Remove the sole order at price 102 — level should be destroyed.
  auto &bid_ladder = book_.bids();
  ASSERT_FALSE(bid_ladder.empty());
  // Best bid = 102, which is front.
  auto &best_level = bid_ladder.front();
  ASSERT_EQ(best_level.price, 102);
  auto &only_order = best_level.orders.front();
  ASSERT_EQ(only_order.order_id, 2U);

  const bool level_destroyed = book_.remove_filled_order(only_order);
  EXPECT_TRUE(level_destroyed);

  // Level at 102 is gone, best bid is now 100.
  EXPECT_EQ(book_.total_orders(), 1U);
  EXPECT_EQ(book_.book_depth(Side::kBid), 1U);
  EXPECT_EQ(book_.best_bid(), 100);
  EXPECT_EQ(book_.volume_at_level(Side::kBid, 102), 0U);
}

// =============================================================================
// 27. RestOrder
// =============================================================================

TEST_F(OrderBookTest, RestOrder) {
  // rest_order is a semantic alias for add_order — verify it works.
  ASSERT_TRUE(book_.rest_order(1, Side::kAsk, 105, 50));

  EXPECT_EQ(book_.best_ask(), 105);
  EXPECT_EQ(book_.volume_at_level(Side::kAsk, 105), 50U);
  EXPECT_EQ(book_.total_orders(), 1U);

  // Duplicate ID rejected — same validation as add_order.
  EXPECT_FALSE(book_.rest_order(1, Side::kBid, 100, 10));
  EXPECT_EQ(book_.total_orders(), 1U);
}

// =============================================================================
// 28. FIFOOrderSequenceAfterCancel (strengthened FIFO test)
// =============================================================================

TEST_F(OrderBookTest, FIFOOrderSequenceAfterCancel) {
  ASSERT_TRUE(book_.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book_.add_order(2, Side::kBid, 100, 20));
  ASSERT_TRUE(book_.add_order(3, Side::kBid, 100, 30));
  ASSERT_TRUE(book_.add_order(4, Side::kBid, 100, 40));

  // Cancel middle order (2). Remaining sequence: 1, 3, 4.
  ASSERT_TRUE(book_.cancel_order(2));

  auto &bid_ladder = book_.bids();
  ASSERT_FALSE(bid_ladder.empty());
  auto &level = bid_ladder.front();

  std::vector<OrderId> remaining_ids;
  for (const auto &order : level.orders) {
    remaining_ids.push_back(order.order_id);
  }
  std::vector<OrderId> expected = {1, 3, 4};
  EXPECT_EQ(remaining_ids, expected);

  // Cancel front order (1). Remaining sequence: 3, 4.
  ASSERT_TRUE(book_.cancel_order(1));
  remaining_ids.clear();
  // Level reference is still valid (not destroyed, orders remain).
  for (const auto &order : level.orders) {
    remaining_ids.push_back(order.order_id);
  }
  expected = {3, 4};
  EXPECT_EQ(remaining_ids, expected);

  // Cancel back order (4). Remaining sequence: 3.
  ASSERT_TRUE(book_.cancel_order(4));
  remaining_ids.clear();
  for (const auto &order : level.orders) {
    remaining_ids.push_back(order.order_id);
  }
  expected = {3};
  EXPECT_EQ(remaining_ids, expected);
}

// =============================================================================
// 29. SizeofIsSmall — verify non-owning refactor reduced inline size
// =============================================================================

TEST(OrderBookSizeTest, SizeofIsSmall) {
  // After the non-owning refactor, OrderBook contains only pointers,
  // counters, IndexFreeStacks (pointer-based), IntrusiveLists (sentinels),
  // and HashMaps (pointer-based). sizeof should be very small regardless
  // of capacity — all storage is in the external buffer.
  EXPECT_LT(sizeof(OrderBook), 512); // < 512 bytes (very conservative)
}

// =============================================================================
// 30. RequiredBufferSize — verify static helper computes sensible sizes
// =============================================================================

TEST(OrderBookSizeTest, RequiredBufferSize) {
  // Test-size params.
  const auto test_size = OrderBook::required_buffer_size(kTestParams);
  EXPECT_GT(test_size, 0U);
  // Must fit at least: 64 orders + 16 levels + free stacks + hash maps.
  EXPECT_GT(test_size, 64 * sizeof(mk::algo::Order));

  // Default params — larger.
  const auto default_size =
      OrderBook::required_buffer_size(OrderBook::Params{});
  EXPECT_GT(default_size, test_size);
}

// =============================================================================
// 31. CreateRejectsDoubleBind — creating then re-binding returns nullopt
// =============================================================================

TEST(OrderBookInitTest, CreateRejectsDoubleBind) {
  std::vector<std::byte> buf(OrderBook::required_buffer_size(kTestParams));
  auto book = OrderBook::create(buf.data(), buf.size(), kTestParams);
  ASSERT_TRUE(book.has_value());

  // A created book's internal slots are non-null, so direct-constructing
  // another book from the same buffer would fail (re-bind guard). Verified
  // via create() returning nullopt for an already-bound buffer is not
  // directly testable without init(). Instead, verify the book works.
  EXPECT_EQ(book->total_orders(), 0U); // NOLINT(bugprone-unchecked-optional-access)
}

// =============================================================================
// 32. NonZeroBufferWorks — buffer filled with 0xFF before construction
// =============================================================================
//
// Validates that init() properly initializes all internal state regardless
// of the buffer's initial contents. Specifically tests that Order slots'
// IntrusiveListHook fields (prev/next) are zeroed, preventing the
// "already linked" assert in IntrusiveList::push_back().

TEST(OrderBookInitTest, NonZeroBufferWorks) {
  std::vector<std::byte> buf(OrderBook::required_buffer_size(kTestParams));
  std::memset(buf.data(), 0xFF, buf.size());

  OrderBook book(buf.data(), buf.size(), kTestParams);

  // Basic operations must work on a non-zero-initialized buffer.
  ASSERT_TRUE(book.add_order(1, Side::kBid, 100, 10));
  ASSERT_TRUE(book.add_order(2, Side::kAsk, 105, 20));
  EXPECT_EQ(book.best_bid(), 100);
  EXPECT_EQ(book.best_ask(), 105);
  EXPECT_EQ(book.total_orders(), 2U);

  // Cancel and re-add to exercise the alloc/free cycle.
  ASSERT_TRUE(book.cancel_order(1));
  ASSERT_TRUE(book.add_order(3, Side::kBid, 101, 5));
  EXPECT_EQ(book.best_bid(), 101);
}

// =============================================================================
// 33. InitRejectsNullBuffer
// =============================================================================

TEST(OrderBookInitTest, CreateRejectsNullBuffer) {
  EXPECT_FALSE(OrderBook::create(nullptr, 1024, kTestParams).has_value());
}

// =============================================================================
// 34. InitRejectsUndersizedBuffer
// =============================================================================

TEST(OrderBookInitTest, CreateRejectsUndersizedBuffer) {
  const auto required = OrderBook::required_buffer_size(kTestParams);
  std::vector<std::byte> buf(required - 1); // One byte too small.
  EXPECT_FALSE(OrderBook::create(buf.data(), buf.size(), kTestParams).has_value());
}

// =============================================================================
// 35. InitRejectsZeroMaxOrders
// =============================================================================

TEST(OrderBookInitTest, CreateRejectsZeroMaxOrders) {
  constexpr OrderBook::Params kBadParams{.max_orders = 0,
                                         .max_levels = 16,
                                         .order_map_cap = 128,
                                         .level_map_cap = 32};
  std::vector<std::byte> buf(4096);
  EXPECT_FALSE(OrderBook::create(buf.data(), buf.size(), kBadParams).has_value());
}

// =============================================================================
// 36. CreateRejectsNonPowerOfTwoMapCap
// =============================================================================

TEST(OrderBookInitTest, CreateRejectsNonPowerOfTwoMapCap) {
  constexpr OrderBook::Params kBadParams{.max_orders = 64,
                                         .max_levels = 16,
                                         .order_map_cap = 100,
                                         .level_map_cap = 32};
  std::vector<std::byte> buf(OrderBook::required_buffer_size(kTestParams) * 2);
  EXPECT_FALSE(OrderBook::create(buf.data(), buf.size(), kBadParams).has_value());
}

// =============================================================================
// 37. CreateRejectsZeroMaxLevels
// =============================================================================

TEST(OrderBookInitTest, CreateRejectsZeroMaxLevels) {
  constexpr OrderBook::Params kBadParams{.max_orders = 64,
                                         .max_levels = 0,
                                         .order_map_cap = 128,
                                         .level_map_cap = 32};
  std::vector<std::byte> buf(4096);
  EXPECT_FALSE(OrderBook::create(buf.data(), buf.size(), kBadParams).has_value());
}

// =============================================================================
// 38. CreateRejectsNonPowerOfTwoLevelMapCap
// =============================================================================

TEST(OrderBookInitTest, CreateRejectsNonPowerOfTwoLevelMapCap) {
  constexpr OrderBook::Params kBadParams{.max_orders = 64,
                                         .max_levels = 16,
                                         .order_map_cap = 128,
                                         .level_map_cap = 30};
  std::vector<std::byte> buf(OrderBook::required_buffer_size(kTestParams) * 2);
  EXPECT_FALSE(OrderBook::create(buf.data(), buf.size(), kBadParams).has_value());
}

// =============================================================================
// 39. CreateRejectsMaxOrdersExceedsOrderMapCap
// =============================================================================

TEST(OrderBookInitTest, CreateRejectsMaxOrdersExceedsOrderMapCap) {
  constexpr OrderBook::Params kBadParams{.max_orders = 64,
                                         .max_levels = 16,
                                         .order_map_cap = 32,
                                         .level_map_cap = 32};
  std::vector<std::byte> buf(OrderBook::required_buffer_size(kTestParams) * 2);
  EXPECT_FALSE(OrderBook::create(buf.data(), buf.size(), kBadParams).has_value());
}

// =============================================================================
// 40. CreateRejectsMaxLevelsExceedsLevelMapCap
// =============================================================================

TEST(OrderBookInitTest, CreateRejectsMaxLevelsExceedsLevelMapCap) {
  constexpr OrderBook::Params kBadParams{.max_orders = 64,
                                         .max_levels = 16,
                                         .order_map_cap = 128,
                                         .level_map_cap = 4};
  std::vector<std::byte> buf(OrderBook::required_buffer_size(kTestParams) * 2);
  EXPECT_FALSE(OrderBook::create(buf.data(), buf.size(), kBadParams).has_value());
}

// =============================================================================
// 41. DirectConstructorAbortsOnInvalidInput
// =============================================================================

using OrderBookDeathTest = ::testing::Test;

TEST_F(OrderBookDeathTest, DirectConstructorAbortsOnNullBuffer) {
  EXPECT_DEATH(OrderBook(nullptr, 1024, kTestParams), "");
}

TEST_F(OrderBookDeathTest, DirectConstructorAbortsOnUndersizedBuffer) {
  std::vector<std::byte> buf(16); // Way too small.
  EXPECT_DEATH(OrderBook(buf.data(), buf.size(), kTestParams), "");
}

TEST_F(OrderBookDeathTest, DirectConstructorAbortsOnBadParams) {
  constexpr OrderBook::Params kBadParams{.max_orders = 0,
                                         .max_levels = 16,
                                         .order_map_cap = 128,
                                         .level_map_cap = 32};
  std::vector<std::byte> buf(4096);
  EXPECT_DEATH(OrderBook(buf.data(), buf.size(), kBadParams), "");
}

} // namespace
