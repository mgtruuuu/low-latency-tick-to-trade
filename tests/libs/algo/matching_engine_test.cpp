/**
 * @file matching_engine_test.cpp
 * @brief Tests for mk::algo::MatchingEngine — order matching with crossing.
 */

#include "algo/matching_engine.hpp"

#include <gtest/gtest.h>

namespace {

using mk::algo::MatchingEngine;
using mk::algo::OrderId;
using mk::algo::Price;
using mk::algo::Qty;
using mk::algo::Side;

using TestEngine =
    MatchingEngine</*MaxFills=*/32, /*MaxOrders=*/64, /*MaxLevels=*/16,
                   /*OrderMapCap=*/128, /*LevelMapCap=*/32>;

// =============================================================================
// 1. EmptyBookNoMatch — order rests immediately
// =============================================================================

TEST(MatchingEngineTest, EmptyBookNoMatch) {
  TestEngine engine;

  auto result = engine.submit_order(1, Side::kBid, 100, 10);

  EXPECT_TRUE(result.fills.empty());
  EXPECT_EQ(result.remaining_qty, 10U);
  EXPECT_TRUE(result.rested);

  EXPECT_EQ(engine.book().best_bid(), 100);
  EXPECT_EQ(engine.book().total_orders(), 1U);
}

// =============================================================================
// 2. NoMatchBelowSpread — buy below best ask
// =============================================================================

TEST(MatchingEngineTest, NoMatchBelowSpread) {
  TestEngine engine;

  // Rest an ask at 105.
  (void)engine.submit_order(1, Side::kAsk, 105, 10);

  // Submit buy at 100 — does NOT cross 105.
  auto result = engine.submit_order(2, Side::kBid, 100, 5);

  EXPECT_TRUE(result.fills.empty());
  EXPECT_TRUE(result.rested);
  EXPECT_EQ(engine.book().best_bid(), 100);
  EXPECT_EQ(engine.book().best_ask(), 105);
  EXPECT_EQ(engine.book().total_orders(), 2U);
}

// =============================================================================
// 3. ExactMatch — one fill, both orders fully consumed
// =============================================================================

TEST(MatchingEngineTest, ExactMatch) {
  TestEngine engine;

  (void)engine.submit_order(1, Side::kAsk, 100, 10);

  auto result = engine.submit_order(2, Side::kBid, 100, 10);

  ASSERT_EQ(result.fills.size(), 1U);
  EXPECT_EQ(result.fills[0].maker_id, 1U);
  EXPECT_EQ(result.fills[0].taker_id, 2U);
  EXPECT_EQ(result.fills[0].price, 100);
  EXPECT_EQ(result.fills[0].qty, 10U);
  EXPECT_EQ(result.remaining_qty, 0U);
  EXPECT_FALSE(result.rested); // Fully filled, nothing to rest.

  // Both orders consumed — book should be empty.
  EXPECT_EQ(engine.book().total_orders(), 0U);
}

// =============================================================================
// 4. PartialFill — resting order partially filled, taker fully filled
// =============================================================================

TEST(MatchingEngineTest, PartialFill) {
  TestEngine engine;

  (void)engine.submit_order(1, Side::kAsk, 100, 20);

  auto result = engine.submit_order(2, Side::kBid, 100, 5);

  ASSERT_EQ(result.fills.size(), 1U);
  EXPECT_EQ(result.fills[0].qty, 5U);
  EXPECT_EQ(result.remaining_qty, 0U);
  EXPECT_FALSE(result.rested);

  // Resting order should have 15 remaining.
  EXPECT_EQ(engine.book().volume_at_level(Side::kAsk, 100), 15U);
  EXPECT_EQ(engine.book().total_orders(), 1U);
}

// =============================================================================
// 5. MultipleFills — crosses multiple price levels
// =============================================================================

TEST(MatchingEngineTest, MultipleFills) {
  TestEngine engine;

  // Rest asks at 100, 101, 102.
  (void)engine.submit_order(1, Side::kAsk, 100, 5);
  (void)engine.submit_order(2, Side::kAsk, 101, 5);
  (void)engine.submit_order(3, Side::kAsk, 102, 5);

  // Buy 12 at limit 102 — should fill 5@100, 5@101, 2@102.
  auto result = engine.submit_order(4, Side::kBid, 102, 12);

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
  EXPECT_EQ(engine.book().volume_at_level(Side::kAsk, 102), 3U);
  EXPECT_EQ(engine.book().total_orders(), 1U);
}

// =============================================================================
// 6. PriceImprovement — fill at resting price, not aggressor's
// =============================================================================

TEST(MatchingEngineTest, PriceImprovement) {
  TestEngine engine;

  (void)engine.submit_order(1, Side::kAsk, 100, 10);

  // Buy at 105 — should fill at 100 (maker's price), not 105.
  auto result = engine.submit_order(2, Side::kBid, 105, 10);

  ASSERT_EQ(result.fills.size(), 1U);
  EXPECT_EQ(result.fills[0].price, 100); // Price improvement for buyer.
}

// =============================================================================
// 7. FIFOPriority — same price, first resting order matched first
// =============================================================================

TEST(MatchingEngineTest, FIFOPriority) {
  TestEngine engine;

  // Three asks at same price, different IDs.
  (void)engine.submit_order(1, Side::kAsk, 100, 3);
  (void)engine.submit_order(2, Side::kAsk, 100, 3);
  (void)engine.submit_order(3, Side::kAsk, 100, 3);

  // Buy 5 — should fill 3 from order 1, then 2 from order 2.
  auto result = engine.submit_order(4, Side::kBid, 100, 5);

  ASSERT_EQ(result.fills.size(), 2U);
  EXPECT_EQ(result.fills[0].maker_id, 1U);
  EXPECT_EQ(result.fills[0].qty, 3U);
  EXPECT_EQ(result.fills[1].maker_id, 2U);
  EXPECT_EQ(result.fills[1].qty, 2U);

  // Order 2 has 1 remaining, order 3 untouched.
  EXPECT_EQ(engine.book().volume_at_level(Side::kAsk, 100), 4U);
  EXPECT_EQ(engine.book().order_count_at_level(Side::kAsk, 100), 2U);
}

// =============================================================================
// 8. RemainingRests — unfilled portion rests in the book
// =============================================================================

TEST(MatchingEngineTest, RemainingRests) {
  TestEngine engine;

  (void)engine.submit_order(1, Side::kAsk, 100, 5);

  // Buy 15 at 100 — fills 5, rests 10.
  auto result = engine.submit_order(2, Side::kBid, 100, 15);

  ASSERT_EQ(result.fills.size(), 1U);
  EXPECT_EQ(result.fills[0].qty, 5U);
  EXPECT_EQ(result.remaining_qty, 10U);
  EXPECT_TRUE(result.rested);

  EXPECT_EQ(engine.book().best_bid(), 100);
  EXPECT_EQ(engine.book().volume_at_level(Side::kBid, 100), 10U);
}

// =============================================================================
// 9. SellSideCrossing — sell order crosses resting bids
// =============================================================================

TEST(MatchingEngineTest, SellSideCrossing) {
  TestEngine engine;

  // Rest bids at 100, 99, 98.
  (void)engine.submit_order(1, Side::kBid, 100, 5);
  (void)engine.submit_order(2, Side::kBid, 99, 5);
  (void)engine.submit_order(3, Side::kBid, 98, 5);

  // Sell 8 at limit 99 — should fill 5@100, 3@99.
  auto result = engine.submit_order(4, Side::kAsk, 99, 8);

  ASSERT_EQ(result.fills.size(), 2U);
  EXPECT_EQ(result.fills[0].maker_id, 1U);
  EXPECT_EQ(result.fills[0].price, 100); // Best bid filled first.
  EXPECT_EQ(result.fills[0].qty, 5U);
  EXPECT_EQ(result.fills[1].maker_id, 2U);
  EXPECT_EQ(result.fills[1].price, 99);
  EXPECT_EQ(result.fills[1].qty, 3U);

  EXPECT_EQ(result.remaining_qty, 0U);
  EXPECT_EQ(engine.book().volume_at_level(Side::kBid, 99), 2U);
}

// =============================================================================
// 10. LevelCleanup — empty level removed after full match
// =============================================================================

TEST(MatchingEngineTest, LevelCleanup) {
  TestEngine engine;

  (void)engine.submit_order(1, Side::kAsk, 100, 10);

  auto result = engine.submit_order(2, Side::kBid, 100, 10);

  ASSERT_EQ(result.fills.size(), 1U);

  // Price level at 100 should be gone.
  EXPECT_EQ(engine.book().book_depth(Side::kAsk), 0U);
  EXPECT_EQ(engine.book().volume_at_level(Side::kAsk, 100), 0U);
}

// =============================================================================
// 11. CancelOrder — cancel via engine delegates to book
// =============================================================================

TEST(MatchingEngineTest, CancelOrder) {
  TestEngine engine;

  (void)engine.submit_order(1, Side::kBid, 100, 10);

  EXPECT_TRUE(engine.cancel_order(1));
  EXPECT_EQ(engine.book().total_orders(), 0U);
  EXPECT_FALSE(engine.cancel_order(1)); // Already cancelled.
}

// =============================================================================
// 12. ZeroQuantityOrder — rejected with empty result
// =============================================================================

TEST(MatchingEngineTest, ZeroQuantityOrder) {
  TestEngine engine;

  auto result = engine.submit_order(1, Side::kBid, 100, 0);

  EXPECT_TRUE(result.fills.empty());
  EXPECT_EQ(result.remaining_qty, 0U);
  EXPECT_FALSE(result.rested);

  // Span should still point into the engine's internal buffer, not nullptr.
  // This maintains the documented invariant that fills is always a span into
  // the engine's buffer, even when empty.
  EXPECT_NE(result.fills.data(), nullptr);

  // Book should remain empty — zero-qty order was not rested.
  EXPECT_EQ(engine.book().total_orders(), 0U);
}

// =============================================================================
// 13. FillBufferExhaustion — matching stops when MaxFills is reached
// =============================================================================
//
// TestEngine has MaxFills=32, MaxOrders=64. Place 40 resting asks (each qty=1)
// and submit a buy for qty=40. Matching must stop after 32 fills. The remaining
// 8 qty rests in the book, and exactly 32 fills are reported.

TEST(MatchingEngineTest, FillBufferExhaustion) {
  TestEngine engine;

  constexpr Qty kOrdersToPlace = 40;
  constexpr Qty kMaxFills = 32; // Must match TestEngine's MaxFills.

  // Place 40 resting asks at price 100, each with qty=1.
  for (OrderId i = 1; i <= kOrdersToPlace; ++i) {
    auto rest_result = engine.submit_order(i, Side::kAsk, 100, 1);
    ASSERT_TRUE(rest_result.rested) << "Failed to rest order " << i;
  }
  ASSERT_EQ(engine.book().total_orders(), kOrdersToPlace);

  // Submit aggressive buy for all 40 at price 100.
  auto result = engine.submit_order(100, Side::kBid, 100, kOrdersToPlace);

  // Exactly MaxFills fills should be recorded — matching stops at the cap.
  ASSERT_EQ(result.fills.size(), kMaxFills);

  // Each fill should be qty=1 at price 100.
  Qty total_filled = 0;
  for (const auto & fill : result.fills) {
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
  EXPECT_EQ(engine.book().volume_at_level(Side::kAsk, 100), unfilled_asks);
  EXPECT_EQ(engine.book().volume_at_level(Side::kBid, 100), 0U);
  EXPECT_EQ(engine.book().total_orders(), unfilled_asks);
}

// =============================================================================
// 14. PartialFillCrossesSpreadAndRests — remainder rests at aggressor's price
// =============================================================================
//
// Aggressor crosses the spread, partially fills, and the unfilled remainder
// rests at the aggressor's limit price — creating a new best on its side.

TEST(MatchingEngineTest, PartialFillCrossesSpreadAndRests) {
  TestEngine engine;

  // Rest an ask at 100.
  (void)engine.submit_order(1, Side::kAsk, 100, 5);

  // Submit a crossing bid at 101 for qty 15.
  // Fills 5 at 100 (maker's price), remainder of 10 rests at 101.
  auto result = engine.submit_order(2, Side::kBid, 101, 15);

  ASSERT_EQ(result.fills.size(), 1U);
  EXPECT_EQ(result.fills[0].maker_id, 1U);
  EXPECT_EQ(result.fills[0].price, 100);
  EXPECT_EQ(result.fills[0].qty, 5U);

  EXPECT_EQ(result.remaining_qty, 10U);
  EXPECT_TRUE(result.rested);

  // Remainder rested at the aggressor's limit price (101), not the fill price.
  EXPECT_EQ(engine.book().best_bid(), 101);
  EXPECT_EQ(engine.book().volume_at_level(Side::kBid, 101), 10U);
  EXPECT_EQ(engine.book().total_orders(), 1U);
}

// =============================================================================
// 15. RemainingFailsToRestWhenBookIsFull — order pool exhaustion
// =============================================================================
//
// Fill the order pool to capacity with non-crossing orders, then submit
// a new order that doesn't cross. Pool is full, so rest_order fails.
// (Partial-fill + rest-failure requires the aggressor to have remaining > 0,
// which only happens when crossing levels are exhausted. At that point,
// fully consumed makers freed slots. The only way to have remaining > 0
// with zero free slots is a non-crossing order on a full pool.)

TEST(MatchingEngineTest, RemainingFailsToRestWhenBookIsFull) {
  using SmallEngine =
      MatchingEngine</*MaxFills=*/8, /*MaxOrders=*/8, /*MaxLevels=*/4,
                     /*OrderMapCap=*/16, /*LevelMapCap=*/8>;
  SmallEngine engine;

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
//
// Complements test #13: when MaxFills is hit but the remaining order does NOT
// cross the next resting level, it is safe to rest (no crossed book).

TEST(MatchingEngineTest, FillBufferExhaustionNonCrossingRests) {
  using SmallFillEngine =
      MatchingEngine</*MaxFills=*/4, /*MaxOrders=*/64, /*MaxLevels=*/16,
                     /*OrderMapCap=*/128, /*LevelMapCap=*/32>;
  SmallFillEngine engine;

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
//
// Complements tests #5 (MultipleFills) and #8 (RemainingRests): an aggressive
// order sweeps through multiple price levels AND still has unfilled quantity
// that rests in the book.

TEST(MatchingEngineTest, MultipleLevelCrossAndRest) {
  TestEngine engine;

  // Rest asks at 100 (qty 5) and 101 (qty 5).
  (void)engine.submit_order(1, Side::kAsk, 100, 5);
  (void)engine.submit_order(2, Side::kAsk, 101, 5);
  ASSERT_EQ(engine.book().total_orders(), 2U);

  // Submit bid at 101 for qty 20 — fills 5@100, 5@101, rests 10 at 101.
  auto result = engine.submit_order(3, Side::kBid, 101, 20);

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
  EXPECT_EQ(engine.book().total_orders(), 1U);
  EXPECT_EQ(engine.book().best_bid(), 101);
  EXPECT_EQ(engine.book().volume_at_level(Side::kBid, 101), 10U);
  EXPECT_EQ(engine.book().book_depth(Side::kAsk), 0U);
}

// =============================================================================
// 18. DuplicateOrderIdRejected — resting with an already-active ID fails
// =============================================================================
//
// If a non-crossing order is submitted with an OrderId that already exists
// in the book, rest_order (→ add_order) should reject the duplicate.

TEST(MatchingEngineTest, DuplicateOrderIdRejected) {
  TestEngine engine;

  // Rest a bid at 90 with ID 1.
  auto first = engine.submit_order(1, Side::kBid, 90, 10);
  ASSERT_TRUE(first.rested);
  ASSERT_EQ(engine.book().total_orders(), 1U);

  // Submit a non-crossing ask at 100 with the SAME ID 1.
  // The order does not cross (100 > 90), so it tries to rest — but ID 1
  // already exists. rest_order should reject it.
  auto second = engine.submit_order(1, Side::kAsk, 100, 5);

  EXPECT_TRUE(second.fills.empty());
  EXPECT_EQ(second.remaining_qty, 5U);
  EXPECT_FALSE(second.rested); // Duplicate ID — rest rejected.

  // Book unchanged: only the original bid at 90.
  EXPECT_EQ(engine.book().total_orders(), 1U);
  EXPECT_EQ(engine.book().best_bid(), 90);
}

// =============================================================================
// 19. DuplicateOrderIdCrossingRejected — crossing order with existing ID
// =============================================================================
//
// If an incoming order uses an OrderId that already exists on the opposite
// side, it must be rejected before matching — not silently produce fills
// where maker_id == taker_id.

TEST(MatchingEngineTest, DuplicateOrderIdCrossingRejected) {
  TestEngine engine;

  // Rest a bid at 100 with ID 1.
  auto first = engine.submit_order(1, Side::kBid, 100, 10);
  ASSERT_TRUE(first.rested);
  ASSERT_EQ(engine.book().total_orders(), 1U);

  // Submit a crossing ask at 100 with the SAME ID 1.
  // This would match against the resting bid, but must be rejected
  // because ID 1 is already in the book.
  auto second = engine.submit_order(1, Side::kAsk, 100, 5);

  EXPECT_TRUE(second.fills.empty());
  EXPECT_EQ(second.remaining_qty, 5U);
  EXPECT_FALSE(second.rested);

  // Book unchanged: the original bid at 100 is untouched.
  EXPECT_EQ(engine.book().total_orders(), 1U);
  EXPECT_EQ(engine.book().best_bid(), 100);
  EXPECT_EQ(engine.book().volume_at_level(Side::kBid, 100), 10U);
}

} // namespace
