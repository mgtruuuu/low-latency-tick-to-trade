/**
 * @file matching_engine_test.cpp
 * @brief Tests for mk::algo::MatchingEngine — order matching with crossing.
 */

#include "algo/matching_engine.hpp"

#include <vector>

#include <gtest/gtest.h>

namespace {

using mk::algo::MatchingEngine;
using mk::algo::OrderBook;
using mk::algo::OrderId;
using mk::algo::Price;
using mk::algo::Qty;
using mk::algo::Side;

// Test-sized params matching the old template defaults for tests.
constexpr OrderBook::Params kTestParams{
    .max_orders = 64,
    .max_levels = 16,
    .order_map_cap = 128,
    .level_map_cap = 32,
};

/// Test fixture with MaxFills=32 and test-sized OrderBook.
class MatchingEngineTest : public ::testing::Test {
protected:
  static constexpr std::size_t kMaxFills = 32;
  using TestEngine = MatchingEngine<kMaxFills>;

  std::vector<std::byte> buf_{TestEngine::required_buffer_size(kTestParams)};
  TestEngine engine_{buf_.data(), buf_.size(), kTestParams};
};

// =============================================================================
// 1. EmptyBookNoMatch — order rests immediately
// =============================================================================

TEST_F(MatchingEngineTest, EmptyBookNoMatch) {
  auto result = engine_.submit_order(1, Side::kBid, 100, 10);

  EXPECT_TRUE(result.fills.empty());
  EXPECT_EQ(result.remaining_qty, 10U);
  EXPECT_TRUE(result.rested);

  EXPECT_EQ(engine_.book().best_bid(), 100);
  EXPECT_EQ(engine_.book().total_orders(), 1U);
}

// =============================================================================
// 2. NoMatchBelowSpread — buy below best ask
// =============================================================================

TEST_F(MatchingEngineTest, NoMatchBelowSpread) {
  // Rest an ask at 105.
  (void)engine_.submit_order(1, Side::kAsk, 105, 10);

  // Submit buy at 100 — does NOT cross 105.
  auto result = engine_.submit_order(2, Side::kBid, 100, 5);

  EXPECT_TRUE(result.fills.empty());
  EXPECT_TRUE(result.rested);
  EXPECT_EQ(engine_.book().best_bid(), 100);
  EXPECT_EQ(engine_.book().best_ask(), 105);
  EXPECT_EQ(engine_.book().total_orders(), 2U);
}

// =============================================================================
// 3. ExactMatch — one fill, both orders fully consumed
// =============================================================================

TEST_F(MatchingEngineTest, ExactMatch) {
  (void)engine_.submit_order(1, Side::kAsk, 100, 10);

  auto result = engine_.submit_order(2, Side::kBid, 100, 10);

  ASSERT_EQ(result.fills.size(), 1U);
  EXPECT_EQ(result.fills[0].maker_id, 1U);
  EXPECT_EQ(result.fills[0].taker_id, 2U);
  EXPECT_EQ(result.fills[0].price, 100);
  EXPECT_EQ(result.fills[0].qty, 10U);
  EXPECT_EQ(result.remaining_qty, 0U);
  EXPECT_FALSE(result.rested); // Fully filled, nothing to rest.

  // Both orders consumed — book should be empty.
  EXPECT_EQ(engine_.book().total_orders(), 0U);
}

// =============================================================================
// 4. PartialFill — resting order partially filled, taker fully filled
// =============================================================================

TEST_F(MatchingEngineTest, PartialFill) {
  (void)engine_.submit_order(1, Side::kAsk, 100, 20);

  auto result = engine_.submit_order(2, Side::kBid, 100, 5);

  ASSERT_EQ(result.fills.size(), 1U);
  EXPECT_EQ(result.fills[0].qty, 5U);
  EXPECT_EQ(result.remaining_qty, 0U);
  EXPECT_FALSE(result.rested);

  // Resting order should have 15 remaining.
  EXPECT_EQ(engine_.book().volume_at_level(Side::kAsk, 100), 15U);
  EXPECT_EQ(engine_.book().total_orders(), 1U);
}

// =============================================================================
// 5. MultipleFills — crosses multiple price levels
// =============================================================================

TEST_F(MatchingEngineTest, MultipleFills) {
  // Rest asks at 100, 101, 102.
  (void)engine_.submit_order(1, Side::kAsk, 100, 5);
  (void)engine_.submit_order(2, Side::kAsk, 101, 5);
  (void)engine_.submit_order(3, Side::kAsk, 102, 5);

  // Buy 12 at limit 102 — should fill 5@100, 5@101, 2@102.
  auto result = engine_.submit_order(4, Side::kBid, 102, 12);

  ASSERT_EQ(result.fills.size(), 3U);

  EXPECT_EQ(result.fills[0].maker_id, 1U);
  EXPECT_EQ(result.fills[0].price, 100);
  EXPECT_EQ(result.fills[0].qty, 5U);

  EXPECT_EQ(result.fills[1].maker_id, 2U);
  EXPECT_EQ(result.fills[1].price, 101);
  EXPECT_EQ(result.fills[1].qty, 5U);

  EXPECT_EQ(result.fills[2].maker_id, 3U);
  EXPECT_EQ(result.fills[2].price, 102);
  EXPECT_EQ(result.fills[2].qty, 2U);

  EXPECT_EQ(result.remaining_qty, 0U);
  EXPECT_FALSE(result.rested);

  // Order 3 should have 3 remaining at price 102.
  EXPECT_EQ(engine_.book().volume_at_level(Side::kAsk, 102), 3U);
  EXPECT_EQ(engine_.book().total_orders(), 1U);
}

// =============================================================================
// 6. PriceImprovement — fill at resting price, not aggressor's
// =============================================================================

TEST_F(MatchingEngineTest, PriceImprovement) {
  (void)engine_.submit_order(1, Side::kAsk, 100, 10);

  // Buy at 105 — should fill at 100 (maker's price), not 105.
  auto result = engine_.submit_order(2, Side::kBid, 105, 10);

  ASSERT_EQ(result.fills.size(), 1U);
  EXPECT_EQ(result.fills[0].price, 100); // Price improvement for buyer.
}

// =============================================================================
// 7. FIFOPriority — same price, first resting order matched first
// =============================================================================

TEST_F(MatchingEngineTest, FIFOPriority) {
  // Three asks at same price, different IDs.
  (void)engine_.submit_order(1, Side::kAsk, 100, 3);
  (void)engine_.submit_order(2, Side::kAsk, 100, 3);
  (void)engine_.submit_order(3, Side::kAsk, 100, 3);

  // Buy 5 — should fill 3 from order 1, then 2 from order 2.
  auto result = engine_.submit_order(4, Side::kBid, 100, 5);

  ASSERT_EQ(result.fills.size(), 2U);
  EXPECT_EQ(result.fills[0].maker_id, 1U);
  EXPECT_EQ(result.fills[0].qty, 3U);
  EXPECT_EQ(result.fills[1].maker_id, 2U);
  EXPECT_EQ(result.fills[1].qty, 2U);

  // Order 2 has 1 remaining, order 3 untouched.
  EXPECT_EQ(engine_.book().volume_at_level(Side::kAsk, 100), 4U);
  EXPECT_EQ(engine_.book().order_count_at_level(Side::kAsk, 100), 2U);
}

// =============================================================================
// 8. RemainingRests — unfilled portion rests in the book
// =============================================================================

TEST_F(MatchingEngineTest, RemainingRests) {
  (void)engine_.submit_order(1, Side::kAsk, 100, 5);

  // Buy 15 at 100 — fills 5, rests 10.
  auto result = engine_.submit_order(2, Side::kBid, 100, 15);

  ASSERT_EQ(result.fills.size(), 1U);
  EXPECT_EQ(result.fills[0].qty, 5U);
  EXPECT_EQ(result.remaining_qty, 10U);
  EXPECT_TRUE(result.rested);

  EXPECT_EQ(engine_.book().best_bid(), 100);
  EXPECT_EQ(engine_.book().volume_at_level(Side::kBid, 100), 10U);
}

// =============================================================================
// 9. SellSideCrossing — sell order crosses resting bids
// =============================================================================

TEST_F(MatchingEngineTest, SellSideCrossing) {
  // Rest bids at 100, 99, 98.
  (void)engine_.submit_order(1, Side::kBid, 100, 5);
  (void)engine_.submit_order(2, Side::kBid, 99, 5);
  (void)engine_.submit_order(3, Side::kBid, 98, 5);

  // Sell 8 at limit 99 — should fill 5@100, 3@99.
  auto result = engine_.submit_order(4, Side::kAsk, 99, 8);

  ASSERT_EQ(result.fills.size(), 2U);
  EXPECT_EQ(result.fills[0].maker_id, 1U);
  EXPECT_EQ(result.fills[0].price, 100); // Best bid filled first.
  EXPECT_EQ(result.fills[0].qty, 5U);
  EXPECT_EQ(result.fills[1].maker_id, 2U);
  EXPECT_EQ(result.fills[1].price, 99);
  EXPECT_EQ(result.fills[1].qty, 3U);

  EXPECT_EQ(result.remaining_qty, 0U);
  EXPECT_EQ(engine_.book().volume_at_level(Side::kBid, 99), 2U);
}

// =============================================================================
// 10. LevelCleanup — empty level removed after full match
// =============================================================================

TEST_F(MatchingEngineTest, LevelCleanup) {
  (void)engine_.submit_order(1, Side::kAsk, 100, 10);

  auto result = engine_.submit_order(2, Side::kBid, 100, 10);

  ASSERT_EQ(result.fills.size(), 1U);

  // Price level at 100 should be gone.
  EXPECT_EQ(engine_.book().book_depth(Side::kAsk), 0U);
  EXPECT_EQ(engine_.book().volume_at_level(Side::kAsk, 100), 0U);
}

// =============================================================================
// 11. CancelOrder — cancel via engine delegates to book
// =============================================================================

TEST_F(MatchingEngineTest, CancelOrder) {
  (void)engine_.submit_order(1, Side::kBid, 100, 10);

  EXPECT_TRUE(engine_.cancel_order(1));
  EXPECT_EQ(engine_.book().total_orders(), 0U);
  EXPECT_FALSE(engine_.cancel_order(1)); // Already cancelled.
}

// =============================================================================
// 12. ZeroQuantityOrder — rejected with empty result
// =============================================================================

TEST_F(MatchingEngineTest, ZeroQuantityOrder) {
  auto result = engine_.submit_order(1, Side::kBid, 100, 0);

  EXPECT_TRUE(result.fills.empty());
  EXPECT_EQ(result.remaining_qty, 0U);
  EXPECT_FALSE(result.rested);

  // Span should still point into the engine's internal buffer, not nullptr.
  // This maintains the documented invariant that fills is always a span into
  // the engine's buffer, even when empty.
  EXPECT_NE(result.fills.data(), nullptr);

  // Book should remain empty — zero-qty order was not rested.
  EXPECT_EQ(engine_.book().total_orders(), 0U);
}

// =============================================================================
// 13. FillBufferExhaustion — matching stops when MaxFills is reached
// =============================================================================
//
// TestEngine has MaxFills=32, MaxOrders=64. Place 40 resting asks (each qty=1)
// and submit a buy for qty=40. Matching must stop after 32 fills. The remaining
// 8 qty rests in the book, and exactly 32 fills are reported.

TEST_F(MatchingEngineTest, FillBufferExhaustion) {
  constexpr Qty kOrdersToPlace = 40;

  // Place 40 resting asks at price 100, each with qty=1.
  for (OrderId i = 1; i <= kOrdersToPlace; ++i) {
    auto rest_result = engine_.submit_order(i, Side::kAsk, 100, 1);
    ASSERT_TRUE(rest_result.rested) << "Failed to rest order " << i;
  }
  ASSERT_EQ(engine_.book().total_orders(), kOrdersToPlace);

  // Submit aggressive buy for all 40 at price 100.
  auto result = engine_.submit_order(100, Side::kBid, 100, kOrdersToPlace);

  // Exactly MaxFills fills should be recorded — matching stops at the cap.
  ASSERT_EQ(result.fills.size(), kMaxFills);

  // Each fill should be qty=1 at price 100.
  Qty total_filled = 0;
  for (const auto &fill : result.fills) {
    EXPECT_EQ(fill.qty, 1U);
    EXPECT_EQ(fill.price, 100);
    EXPECT_EQ(fill.taker_id, 100U);
    total_filled += fill.qty;
  }
  EXPECT_EQ(total_filled, kMaxFills);

  // Remaining quantity = 40 - 32 = 8. Matching stopped because fill buffer
  // was exhausted, NOT because we ran out of crossing orders. Resting the
  // remainder would create a crossed book (bid at 100 while asks at 100 exist).
  // The engine must refuse to rest in this situation.
  EXPECT_EQ(result.remaining_qty, kOrdersToPlace - kMaxFills);
  EXPECT_FALSE(result.rested);

  // Book state: only the 8 unfilled asks remain. No bid was rested.
  const Qty unfilled_asks = kOrdersToPlace - kMaxFills;
  EXPECT_EQ(engine_.book().volume_at_level(Side::kAsk, 100), unfilled_asks);
  EXPECT_EQ(engine_.book().volume_at_level(Side::kBid, 100), 0U);
  EXPECT_EQ(engine_.book().total_orders(), unfilled_asks);
}

// =============================================================================
// 14. PartialFillCrossesSpreadAndRests — remainder rests at aggressor's price
// =============================================================================
//
// Aggressor crosses the spread, partially fills, and the unfilled remainder
// rests at the aggressor's limit price — creating a new best on its side.

TEST_F(MatchingEngineTest, PartialFillCrossesSpreadAndRests) {
  // Rest an ask at 100.
  (void)engine_.submit_order(1, Side::kAsk, 100, 5);

  // Submit a crossing bid at 101 for qty 15.
  // Fills 5 at 100 (maker's price), remainder of 10 rests at 101.
  auto result = engine_.submit_order(2, Side::kBid, 101, 15);

  ASSERT_EQ(result.fills.size(), 1U);
  EXPECT_EQ(result.fills[0].maker_id, 1U);
  EXPECT_EQ(result.fills[0].price, 100);
  EXPECT_EQ(result.fills[0].qty, 5U);

  EXPECT_EQ(result.remaining_qty, 10U);
  EXPECT_TRUE(result.rested);

  // Remainder rested at the aggressor's limit price (101), not the fill price.
  EXPECT_EQ(engine_.book().best_bid(), 101);
  EXPECT_EQ(engine_.book().volume_at_level(Side::kBid, 101), 10U);
  EXPECT_EQ(engine_.book().total_orders(), 1U);
}

// =============================================================================
// 15. RemainingFailsToRestWhenBookIsFull — order pool exhaustion
// =============================================================================

TEST(MatchingEngineSmallTest, RemainingFailsToRestWhenBookIsFull) {
  constexpr OrderBook::Params kSmallParams{
      .max_orders = 8,
      .max_levels = 4,
      .order_map_cap = 16,
      .level_map_cap = 8,
  };
  using SmallEngine = MatchingEngine<8>;

  std::vector<std::byte> buf(SmallEngine::required_buffer_size(kSmallParams));
  SmallEngine engine(buf.data(), buf.size(), kSmallParams);

  // Fill all 8 order slots: 4 asks at 200, 4 bids at 100 (no crossing).
  for (OrderId i = 1; i <= 4; ++i) {
    auto r = engine.submit_order(i, Side::kAsk, 200, 10);
    ASSERT_TRUE(r.rested) << "Failed to rest ask " << i;
  }
  for (OrderId i = 5; i <= 8; ++i) {
    auto r = engine.submit_order(i, Side::kBid, 100, 10);
    ASSERT_TRUE(r.rested) << "Failed to rest bid " << i;
  }
  ASSERT_EQ(engine.book().total_orders(), 8U);
  ASSERT_EQ(engine.book().free_order_count(), 0U);

  // Submit a bid at 99 — doesn't cross best ask (200). Pool is full.
  auto result = engine.submit_order(100, Side::kBid, 99, 5);

  EXPECT_TRUE(result.fills.empty());
  EXPECT_EQ(result.remaining_qty, 5U);
  EXPECT_FALSE(result.rested); // Pool exhausted — can't rest.
  EXPECT_EQ(engine.book().total_orders(), 8U);
}

// =============================================================================
// 16. FillBufferExhaustionNonCrossingRests — remainder rests when it no longer
//     crosses the opposite side after MaxFills is hit
// =============================================================================

TEST(MatchingEngineSmallTest, FillBufferExhaustionNonCrossingRests) {
  constexpr OrderBook::Params kParams{
      .max_orders = 64,
      .max_levels = 16,
      .order_map_cap = 128,
      .level_map_cap = 32,
  };
  using SmallFillEngine = MatchingEngine<4>;

  std::vector<std::byte> buf(SmallFillEngine::required_buffer_size(kParams));
  SmallFillEngine engine(buf.data(), buf.size(), kParams);

  // Rest 4 asks at price 100 (qty 1 each) and 1 ask at price 200.
  for (OrderId i = 1; i <= 4; ++i) {
    (void)engine.submit_order(i, Side::kAsk, 100, 1);
  }
  (void)engine.submit_order(5, Side::kAsk, 200, 10);

  // Submit a bid at 150 for qty 10. Crosses 4 asks at 100 (4 fills, hitting
  // MaxFills). Remaining = 6. The bid at 150 does NOT cross the ask at 200,
  // so it's safe to rest.
  auto result = engine.submit_order(10, Side::kBid, 150, 10);

  ASSERT_EQ(result.fills.size(), 4U);
  EXPECT_EQ(result.remaining_qty, 6U);
  EXPECT_TRUE(result.rested);

  // Remainder rested at 150 (below best ask of 200). Book is not crossed.
  EXPECT_EQ(engine.book().best_bid(), 150);
  EXPECT_EQ(engine.book().best_ask(), 200);
  EXPECT_EQ(engine.book().volume_at_level(Side::kBid, 150), 6U);
}

// =============================================================================
// 17. MultipleLevelCrossAndRest — crosses multiple levels, remainder rests
// =============================================================================

TEST_F(MatchingEngineTest, MultipleLevelCrossAndRest) {
  // Rest asks at 100 (qty 5) and 101 (qty 5).
  (void)engine_.submit_order(1, Side::kAsk, 100, 5);
  (void)engine_.submit_order(2, Side::kAsk, 101, 5);
  ASSERT_EQ(engine_.book().total_orders(), 2U);

  // Submit bid at 101 for qty 20 — fills 5@100, 5@101, rests 10 at 101.
  auto result = engine_.submit_order(3, Side::kBid, 101, 20);

  // Should have 2 fills (one per price level).
  ASSERT_EQ(result.fills.size(), 2U);

  EXPECT_EQ(result.fills[0].maker_id, 1U);
  EXPECT_EQ(result.fills[0].price, 100);
  EXPECT_EQ(result.fills[0].qty, 5U);

  EXPECT_EQ(result.fills[1].maker_id, 2U);
  EXPECT_EQ(result.fills[1].price, 101);
  EXPECT_EQ(result.fills[1].qty, 5U);

  // 10 remaining, rested at the aggressor's limit price (101).
  EXPECT_EQ(result.remaining_qty, 10U);
  EXPECT_TRUE(result.rested);

  // Book state: both asks consumed, one bid resting at 101.
  EXPECT_EQ(engine_.book().total_orders(), 1U);
  EXPECT_EQ(engine_.book().best_bid(), 101);
  EXPECT_EQ(engine_.book().volume_at_level(Side::kBid, 101), 10U);
  EXPECT_EQ(engine_.book().book_depth(Side::kAsk), 0U);
}

// =============================================================================
// 18. DuplicateOrderIdRejected — resting with an already-active ID fails
// =============================================================================

TEST_F(MatchingEngineTest, DuplicateOrderIdRejected) {
  // Rest a bid at 90 with ID 1.
  auto first = engine_.submit_order(1, Side::kBid, 90, 10);
  ASSERT_TRUE(first.rested);
  ASSERT_EQ(engine_.book().total_orders(), 1U);

  // Submit a non-crossing ask at 100 with the SAME ID 1.
  // The order does not cross (100 > 90), so it tries to rest — but ID 1
  // already exists. rest_order should reject it.
  auto second = engine_.submit_order(1, Side::kAsk, 100, 5);

  EXPECT_TRUE(second.fills.empty());
  EXPECT_EQ(second.remaining_qty, 5U);
  EXPECT_FALSE(second.rested); // Duplicate ID — rest rejected.

  // Book unchanged: only the original bid at 90.
  EXPECT_EQ(engine_.book().total_orders(), 1U);
  EXPECT_EQ(engine_.book().best_bid(), 90);
}

// =============================================================================
// 19. DuplicateOrderIdCrossingRejected — crossing order with existing ID
// =============================================================================

TEST_F(MatchingEngineTest, DuplicateOrderIdCrossingRejected) {
  // Rest a bid at 100 with ID 1.
  auto first = engine_.submit_order(1, Side::kBid, 100, 10);
  ASSERT_TRUE(first.rested);
  ASSERT_EQ(engine_.book().total_orders(), 1U);

  // Submit a crossing ask at 100 with the SAME ID 1.
  // This would match against the resting bid, but must be rejected
  // because ID 1 is already in the book.
  auto second = engine_.submit_order(1, Side::kAsk, 100, 5);

  EXPECT_TRUE(second.fills.empty());
  EXPECT_EQ(second.remaining_qty, 5U);
  EXPECT_FALSE(second.rested);

  // Book unchanged: the original bid at 100 is untouched.
  EXPECT_EQ(engine_.book().total_orders(), 1U);
  EXPECT_EQ(engine_.book().best_bid(), 100);
  EXPECT_EQ(engine_.book().volume_at_level(Side::kBid, 100), 10U);
}

// =============================================================================
// 20. ModifyOrder — basic cancel + re-submit
// =============================================================================

TEST_F(MatchingEngineTest, ModifyOrderBasic) {
  // Rest a bid at 90 with ID 1.
  auto first = engine_.submit_order(1, Side::kBid, 90, 10);
  ASSERT_TRUE(first.rested);

  // Modify: cancel old ID 1, re-submit as new ID 2 at price 95 qty 8.
  auto result = engine_.modify_order(1, Side::kBid, 95, 8, 2);

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.fills.empty()); // no crossing
  EXPECT_EQ(result.remaining_qty, 8U);
  EXPECT_TRUE(result.rested);

  // Old order gone, new order resting at 95.
  EXPECT_FALSE(engine_.book().has_order(1));
  EXPECT_TRUE(engine_.book().has_order(2));
  EXPECT_EQ(engine_.book().best_bid(), 95);
  EXPECT_EQ(engine_.book().volume_at_level(Side::kBid, 95), 8U);
}

// =============================================================================
// 21. ModifyOrderNotFound — old order doesn't exist
// =============================================================================

TEST_F(MatchingEngineTest, ModifyOrderNotFound) {
  auto result = engine_.modify_order(999, Side::kBid, 100, 10, 1000);

  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.fills.empty());
  EXPECT_EQ(engine_.book().total_orders(), 0U);
}

// =============================================================================
// 22. ModifyOrderCrossing — modified order crosses opposite side
// =============================================================================

TEST_F(MatchingEngineTest, ModifyOrderCrossing) {
  // Rest an ask at 100.
  (void)engine_.submit_order(1, Side::kAsk, 100, 5);
  // Rest a bid at 90.
  (void)engine_.submit_order(2, Side::kBid, 90, 10);
  ASSERT_EQ(engine_.book().total_orders(), 2U);

  // Modify bid up to 100 — crosses the ask.
  auto result = engine_.modify_order(2, Side::kBid, 100, 10, 3);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(result.fills.size(), 1U);
  EXPECT_EQ(result.fills[0].price, 100);
  EXPECT_EQ(result.fills[0].qty, 5U);
  EXPECT_EQ(result.remaining_qty, 5U);
  EXPECT_TRUE(result.rested); // remainder rests at 100
}

// =============================================================================
// 23. ModifyOrderSuccessButReplacementRejectsZeroQty
// =============================================================================
//
// success=true means the OLD order was cancelled. The replacement's zero qty
// is reflected in remaining_qty/rested, not in the success flag.

TEST_F(MatchingEngineTest, ModifyOrderSuccessButReplacementRejectsZeroQty) {
  (void)engine_.submit_order(1, Side::kBid, 90, 10);
  ASSERT_EQ(engine_.book().total_orders(), 1U);

  // Modify with new_qty=0 — old order cancelled, replacement rejected.
  auto result = engine_.modify_order(1, Side::kBid, 95, 0, 2);

  EXPECT_TRUE(result.success);        // old order WAS found and cancelled
  EXPECT_TRUE(result.fills.empty());
  EXPECT_EQ(result.remaining_qty, 0U); // zero-qty rejection
  EXPECT_FALSE(result.rested);

  // Both old and new orders are gone.
  EXPECT_FALSE(engine_.book().has_order(1));
  EXPECT_FALSE(engine_.book().has_order(2));
  EXPECT_EQ(engine_.book().total_orders(), 0U);
}

// =============================================================================
// 24. ModifyOrderSuccessButReplacementRejectsDuplicateId
// =============================================================================
//
// Old order is cancelled (success=true), but the replacement uses new_id
// that already exists in the book → rest_order rejects the duplicate.
// The old order is gone, the replacement is not rested.

TEST_F(MatchingEngineTest, ModifyOrderSuccessButReplacementRejectsDuplicateId) {
  // Rest two bids: ID 1 at 90, ID 2 at 80.
  (void)engine_.submit_order(1, Side::kBid, 90, 10);
  (void)engine_.submit_order(2, Side::kBid, 80, 5);
  ASSERT_EQ(engine_.book().total_orders(), 2U);

  // Modify ID 1 → new_id=2 (already in the book). Cancel succeeds,
  // replacement is rejected by has_order(2) duplicate guard.
  auto result = engine_.modify_order(1, Side::kBid, 95, 8, 2);

  EXPECT_TRUE(result.success);         // old order WAS cancelled
  EXPECT_TRUE(result.fills.empty());
  EXPECT_EQ(result.remaining_qty, 8U); // duplicate ID rejection
  EXPECT_FALSE(result.rested);         // not rested

  // Old order (ID 1) is gone. Existing order (ID 2) is untouched.
  EXPECT_FALSE(engine_.book().has_order(1));
  EXPECT_TRUE(engine_.book().has_order(2));
  EXPECT_EQ(engine_.book().total_orders(), 1U);
  EXPECT_EQ(engine_.book().best_bid(), 80); // only ID 2 remains
}

// =============================================================================
// 25. ModifyOrderSuccessButReplacementRejectsLevelPoolFull
// =============================================================================
//
// cancel frees an order slot but does NOT free the price level (other orders
// remain at that price). The replacement needs a new level at a different
// price, but the level pool is exhausted → rest_order() fails.
// success=true (old order was cancelled), rested=false (replacement failed).

TEST(MatchingEngineSmallTest,
     ModifyOrderSuccessButReplacementRejectsLevelPoolFull) {
  // max_levels=2: only 2 distinct price levels can exist at once.
  constexpr OrderBook::Params kLevelLimited{
      .max_orders = 8,
      .max_levels = 2,
      .order_map_cap = 16,
      .level_map_cap = 4,
  };
  using SmallEngine = MatchingEngine<8>;

  std::vector<std::byte> buf(SmallEngine::required_buffer_size(kLevelLimited));
  SmallEngine engine(buf.data(), buf.size(), kLevelLimited);

  // Create 2 levels: Bid@90 (2 orders) and Ask@110.
  (void)engine.submit_order(1, Side::kBid, 90, 10);
  (void)engine.submit_order(2, Side::kBid, 90, 5);
  (void)engine.submit_order(3, Side::kAsk, 110, 10);
  ASSERT_EQ(engine.book().total_orders(), 3U);
  ASSERT_EQ(engine.book().book_depth(Side::kBid), 1U); // 1 bid level (90)
  ASSERT_EQ(engine.book().book_depth(Side::kAsk), 1U); // 1 ask level (110)
  ASSERT_EQ(engine.book().free_level_count(), 0U);      // 2/2 used

  // Modify order 1 (Bid@90) → Bid@95 (new price, needs a 3rd level).
  // Cancel frees order slot but level 90 survives (order 2 remains).
  // Replacement needs a new level at 95 → level pool exhausted → fails.
  auto result = engine.modify_order(1, Side::kBid, 95, 10, 100);

  EXPECT_TRUE(result.success);         // old order WAS cancelled
  EXPECT_TRUE(result.fills.empty());   // no crossing
  EXPECT_EQ(result.remaining_qty, 10U);
  EXPECT_FALSE(result.rested);         // level pool full → can't rest

  // Order 1 is gone, order 100 was never created.
  EXPECT_FALSE(engine.book().has_order(1));
  EXPECT_FALSE(engine.book().has_order(100));
  EXPECT_EQ(engine.book().total_orders(), 2U); // only orders 2 and 3 remain
}

// =============================================================================
// 26. ModifyOrderUnderMapPressureMaintainsContract
// =============================================================================
//
// Contract test (not path coverage): verifies that success=true and old-order
// removal hold under HashMap tombstone pressure. Whether the replacement
// actually gets rejected depends on probe landing — HashMap uses deterministic
// linear probing with mix64 hash, so the outcome is key-dependent. This test
// accepts both outcomes and verifies the contract either way.
//
// Pinning the actual order_map_.insert() failure branch would require reverse-
// engineering mix64 to find a key pair where the tombstone slot is unreachable
// by the replacement's probe sequence — possible but brittle across hash
// function changes. The level-pool-full test (#25) already demonstrates a
// deterministic book-full replacement rejection path.

TEST(MatchingEngineSmallTest,
     ModifyOrderUnderMapPressureMaintainsContract) {
  // order_map_cap=8 → max_load = 8 * 7 / 10 = 5.
  // max_orders=8 > 5, so the map is the bottleneck.
  constexpr OrderBook::Params kMapLimited{
      .max_orders = 8,
      .max_levels = 8,
      .order_map_cap = 8,
      .level_map_cap = 16,
  };
  using SmallEngine = MatchingEngine<8>;

  std::vector<std::byte> buf(SmallEngine::required_buffer_size(kMapLimited));
  SmallEngine engine(buf.data(), buf.size(), kMapLimited);

  // Insert 5 orders (hits max_load = 5). All at same price to use 1 level.
  for (OrderId i = 1; i <= 5; ++i) {
    auto r = engine.submit_order(i, Side::kBid, 100, 1);
    ASSERT_TRUE(r.rested) << "Failed to rest order " << i;
  }
  ASSERT_EQ(engine.book().total_orders(), 5U);

  // 6th insert should fail — map at max_load.
  auto overflow = engine.submit_order(6, Side::kBid, 100, 1);
  EXPECT_FALSE(overflow.rested);

  // Modify order 1 → new_id=100.
  // Cancel creates a tombstone: size=4, tombstones=1, total=5 = max_load.
  // Replacement insert: size + tombstones = 5 = max_load.
  // If probe does NOT land on the tombstone → rejected.
  // If probe DOES land on it → tombstone reused, insert succeeds.
  //
  // This is hash-distribution dependent. Use a different ID (100) that
  // is likely to hash to a different bucket than ID 1.
  auto result = engine.modify_order(1, Side::kBid, 100, 1, 100);
  EXPECT_TRUE(result.success); // old order WAS cancelled

  // The replacement may or may not succeed depending on hash distribution.
  // Either way, the contract holds: success=true regardless of replacement.
  // Verify invariant: old order is gone.
  EXPECT_FALSE(engine.book().has_order(1));

  if (result.rested) {
    // Tombstone was reused — replacement succeeded.
    EXPECT_TRUE(engine.book().has_order(100));
    EXPECT_EQ(engine.book().total_orders(), 5U);
  } else {
    // Tombstone was NOT reused — replacement failed (map full).
    EXPECT_FALSE(engine.book().has_order(100));
    EXPECT_EQ(engine.book().total_orders(), 4U);
  }
}

// =============================================================================
// Move / Swap semantics
// =============================================================================

TEST_F(MatchingEngineTest, MoveConstructPreservesBookState) {
  (void)engine_.submit_order(1, Side::kBid, 100, 10);
  (void)engine_.submit_order(2, Side::kAsk, 105, 20);
  ASSERT_EQ(engine_.book().total_orders(), 2U);

  // Move-construct a new engine from the existing one.
  // Use a separate scope to avoid name conflict with fixture member.
  auto moved = std::move(engine_);

  // Moved-to engine has the book state.
  EXPECT_EQ(moved.book().total_orders(), 2U);
  EXPECT_EQ(moved.book().best_bid(), 100);
  EXPECT_EQ(moved.book().best_ask(), 105);

  // Operations work on the moved-to engine.
  auto result = moved.submit_order(3, Side::kBid, 105, 5);
  ASSERT_EQ(result.fills.size(), 1U);
  EXPECT_EQ(result.fills[0].price, 105);
}

TEST_F(MatchingEngineTest, MoveAssignPreservesBookState) {
  (void)engine_.submit_order(1, Side::kBid, 90, 10);

  // Create a second engine with its own buffer.
  std::vector<std::byte> buf2(TestEngine::required_buffer_size(kTestParams));
  TestEngine engine2(buf2.data(), buf2.size(), kTestParams);
  (void)engine2.submit_order(10, Side::kAsk, 200, 50);

  // Move-assign: engine2 gets engine_'s state.
  engine2 = std::move(engine_);

  EXPECT_EQ(engine2.book().total_orders(), 1U);
  EXPECT_EQ(engine2.book().best_bid(), 90);
  // engine2's old state (Ask@200) was destroyed by move assignment.
  EXPECT_EQ(engine2.book().best_ask(), std::nullopt);
}

TEST_F(MatchingEngineTest, SwapExchangesBothEngines) {
  (void)engine_.submit_order(1, Side::kBid, 100, 10);

  std::vector<std::byte> buf2(TestEngine::required_buffer_size(kTestParams));
  TestEngine engine2(buf2.data(), buf2.size(), kTestParams);
  (void)engine2.submit_order(2, Side::kAsk, 200, 20);

  swap(engine_, engine2);

  // engine_ now has engine2's old state.
  EXPECT_EQ(engine_.book().total_orders(), 1U);
  EXPECT_EQ(engine_.book().best_ask(), 200);

  // engine2 now has engine_'s old state.
  EXPECT_EQ(engine2.book().total_orders(), 1U);
  EXPECT_EQ(engine2.book().best_bid(), 100);

  // Both engines remain operational after swap.
  auto r1 = engine_.submit_order(3, Side::kBid, 200, 5);
  EXPECT_EQ(r1.fills.size(), 1U);

  EXPECT_TRUE(engine2.cancel_order(1));
  EXPECT_EQ(engine2.book().total_orders(), 0U);

  // Restore original buffer ownership before scope exit — engine2 uses
  // local buf2, engine_ uses fixture buf_. Without swap-back, engine_
  // would outlive buf2 and access freed memory in its destructor.
  swap(engine_, engine2);
}

// =============================================================================
// MatchResult helper contract tests
// =============================================================================
//
// These tests fix the 3-field combination contract documented in
// matching_engine.hpp. Each outcome must be distinguishable via the
// helper functions without memorizing the raw field table.

TEST_F(MatchingEngineTest, HelperFullyFilled) {
  // Resting ask, then crossing bid for exact qty → fully filled.
  (void)engine_.submit_order(1, Side::kAsk, 100, 10);
  auto r = engine_.submit_order(2, Side::kBid, 100, 10);

  EXPECT_TRUE(r.fully_filled());
  EXPECT_TRUE(r.has_fills());
  EXPECT_FALSE(r.rested_in_book());
  EXPECT_FALSE(r.rejected());
  EXPECT_EQ(r.remaining_qty, 0U);
}

TEST_F(MatchingEngineTest, HelperPartialFillRested) {
  // Resting ask qty=5, crossing bid qty=10 → partial fill + rest remainder.
  (void)engine_.submit_order(1, Side::kAsk, 100, 5);
  auto r = engine_.submit_order(2, Side::kBid, 100, 10);

  EXPECT_FALSE(r.fully_filled());
  EXPECT_TRUE(r.has_fills());
  EXPECT_TRUE(r.rested_in_book());
  EXPECT_FALSE(r.rejected());
  EXPECT_EQ(r.remaining_qty, 5U);
}

TEST_F(MatchingEngineTest, HelperNoMatchRested) {
  // Empty book, order rests immediately.
  auto r = engine_.submit_order(1, Side::kBid, 100, 10);

  EXPECT_FALSE(r.fully_filled());
  EXPECT_FALSE(r.has_fills());
  EXPECT_TRUE(r.rested_in_book());
  EXPECT_FALSE(r.rejected());
  EXPECT_EQ(r.remaining_qty, 10U);
}

TEST_F(MatchingEngineTest, HelperZeroQtyRejected) {
  auto r = engine_.submit_order(1, Side::kBid, 100, 0);

  EXPECT_FALSE(r.fully_filled());
  EXPECT_FALSE(r.has_fills());
  EXPECT_FALSE(r.rested_in_book());
  EXPECT_TRUE(r.rejected());
  EXPECT_EQ(r.remaining_qty, 0U);
}

TEST_F(MatchingEngineTest, HelperDuplicateIdRejected) {
  (void)engine_.submit_order(1, Side::kBid, 100, 10);
  auto r = engine_.submit_order(1, Side::kBid, 100, 10); // duplicate

  EXPECT_FALSE(r.fully_filled());
  EXPECT_FALSE(r.has_fills());
  EXPECT_FALSE(r.rested_in_book());
  EXPECT_TRUE(r.rejected());
  EXPECT_GT(r.remaining_qty, 0U);
}

// Book full — exhaust order pool so rest_order() fails.
// Use a tiny engine (max_orders=2) to trigger this without filling 64 slots.
TEST_F(MatchingEngineTest, HelperBookFullRejected) {
  constexpr OrderBook::Params kTinyParams{
      .max_orders = 2, .max_levels = 4, .order_map_cap = 4, .level_map_cap = 4};
  std::vector<std::byte> buf(
      MatchingEngine<kMaxFills>::required_buffer_size(kTinyParams));
  MatchingEngine<kMaxFills> tiny(buf.data(), buf.size(), kTinyParams);

  // Fill pool: 2 resting orders.
  (void)tiny.submit_order(1, Side::kBid, 100, 10);
  (void)tiny.submit_order(2, Side::kBid, 99, 10);

  // Third order: no crossing (bid below best ask=none), pool exhausted → can't rest.
  auto r = tiny.submit_order(3, Side::kBid, 98, 10);

  EXPECT_FALSE(r.fully_filled());
  EXPECT_FALSE(r.has_fills());
  EXPECT_FALSE(r.rested_in_book());
  EXPECT_TRUE(r.rejected());
  EXPECT_GT(r.remaining_qty, 0U);
}

// Fill buffer exhausted — more resting orders than MaxFills capacity.
// Use MaxFills=2 so we only need 3 resting orders to trigger.
TEST_F(MatchingEngineTest, HelperFillBufferExhausted) {
  constexpr std::size_t kTinyFills = 2;
  using TinyEngine = MatchingEngine<kTinyFills>;
  std::vector<std::byte> buf(TinyEngine::required_buffer_size(kTestParams));
  TinyEngine tiny(buf.data(), buf.size(), kTestParams);

  // 3 resting asks at the same price.
  (void)tiny.submit_order(1, Side::kAsk, 100, 5);
  (void)tiny.submit_order(2, Side::kAsk, 100, 5);
  (void)tiny.submit_order(3, Side::kAsk, 100, 5);

  // Crossing bid for qty=15 — matches all 3, but fill buffer only holds 2.
  // Matching stops after 2 fills. Remainder can't rest (still crosses).
  auto r = tiny.submit_order(4, Side::kBid, 100, 15);

  EXPECT_FALSE(r.fully_filled());
  EXPECT_TRUE(r.has_fills());
  EXPECT_FALSE(r.rested_in_book()); // still crosses, can't rest
  EXPECT_FALSE(r.rejected());
  EXPECT_EQ(r.fills.size(), kTinyFills); // exactly 2 fills
  EXPECT_GT(r.remaining_qty, 0U);
}

// =============================================================================
// ModifyResult helper contract tests
// =============================================================================

TEST_F(MatchingEngineTest, ModifyHelperSuccessRested) {
  // Rest an order, then modify price (no crossing) → success + rested.
  (void)engine_.submit_order(1, Side::kBid, 100, 10);
  auto r = engine_.modify_order(1, Side::kBid, 99, 10, 2);

  EXPECT_TRUE(r.success);
  EXPECT_TRUE(r.rested_in_book());
  EXPECT_TRUE(r.fills.empty());
}

TEST_F(MatchingEngineTest, ModifyHelperSuccessFullyFilled) {
  // Rest a bid, rest an ask, then modify bid price up to cross.
  (void)engine_.submit_order(1, Side::kBid, 100, 10);
  (void)engine_.submit_order(2, Side::kAsk, 105, 10);
  auto r = engine_.modify_order(1, Side::kBid, 105, 10, 3);

  EXPECT_TRUE(r.success);
  EXPECT_FALSE(r.rested_in_book());
  EXPECT_FALSE(r.fills.empty());
  EXPECT_EQ(r.remaining_qty, 0U);
}

TEST_F(MatchingEngineTest, ModifyHelperFailNotFound) {
  // Modify non-existent order → success=false, helpers reflect failure.
  auto r = engine_.modify_order(999, Side::kBid, 100, 10, 1);

  EXPECT_FALSE(r.success);
  EXPECT_FALSE(r.rested_in_book());
}

} // namespace
