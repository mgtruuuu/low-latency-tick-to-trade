#include "tick_to_trade/spread_strategy.hpp"

#include "shared/protocol.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

mk::app::MarketDataUpdate
make_update(std::uint64_t seq, std::uint32_t symbol_id, mk::algo::Side side,
            mk::algo::Price price, mk::algo::Qty qty = 100) {
  return {.seq_num = seq,
          .symbol_id = symbol_id,
          .side = side,
          .price = price,
          .qty = qty,
          .exchange_ts = 0};
}

class SpreadStrategyTest : public ::testing::Test {
protected:
  // Threshold = 1000 ticks. Default qty = 10.
  mk::app::SpreadStrategy<2> strategy_{1000, 10};
  mk::app::Signal signal_{};
};

// -- BBO state --

TEST_F(SpreadStrategyTest, NoSignalOneSideBid) {
  auto md = make_update(1, 1, mk::algo::Side::kBid, 10000);
  EXPECT_FALSE(strategy_.on_market_data(md, signal_));
}

TEST_F(SpreadStrategyTest, NoSignalOneSideAsk) {
  auto md = make_update(1, 1, mk::algo::Side::kAsk, 12000);
  EXPECT_FALSE(strategy_.on_market_data(md, signal_));
}

TEST_F(SpreadStrategyTest, NoSignalNarrowSpread) {
  // Bid=10000, Ask=10500. Spread=500 < threshold=1000.
  EXPECT_FALSE(strategy_.on_market_data(
      make_update(1, 1, mk::algo::Side::kBid, 10000), signal_));
  EXPECT_FALSE(strategy_.on_market_data(
      make_update(2, 1, mk::algo::Side::kAsk, 10500), signal_));
}

TEST_F(SpreadStrategyTest, SignalOnWideSpread) {
  // Bid=10000, Ask=12000. Spread=2000 > threshold=1000.
  EXPECT_FALSE(strategy_.on_market_data(
      make_update(1, 1, mk::algo::Side::kBid, 10000), signal_));
  ASSERT_TRUE(strategy_.on_market_data(
      make_update(2, 1, mk::algo::Side::kAsk, 12000), signal_));

  EXPECT_EQ(signal_.symbol_id, 1U);
  EXPECT_EQ(signal_.qty, 10U);
}

// -- Side alternation and aggressive pricing --

TEST_F(SpreadStrategyTest, AlternatingSidesAndAggressivePricing) {
  // Set up wide spread: bid=10000, ask=12000.
  EXPECT_FALSE(strategy_.on_market_data(
      make_update(1, 1, mk::algo::Side::kBid, 10000), signal_));

  // First signal — buy (buy_next starts true).
  ASSERT_TRUE(strategy_.on_market_data(
      make_update(2, 1, mk::algo::Side::kAsk, 12000), signal_));
  EXPECT_EQ(signal_.side, mk::algo::Side::kBid);
  // Aggressive pricing: buy at the ask.
  EXPECT_EQ(signal_.price, 12000);

  // Second signal — sell (alternated).
  ASSERT_TRUE(strategy_.on_market_data(
      make_update(3, 1, mk::algo::Side::kAsk, 12000), signal_));
  EXPECT_EQ(signal_.side, mk::algo::Side::kAsk);
  // Aggressive pricing: sell at the bid.
  EXPECT_EQ(signal_.price, 10000);
}

// -- Edge cases --

TEST_F(SpreadStrategyTest, CrossedMarketRejection) {
  // Bid=12000, Ask=10000 → crossed (ask <= bid).
  EXPECT_FALSE(strategy_.on_market_data(
      make_update(1, 1, mk::algo::Side::kBid, 12000), signal_));
  EXPECT_FALSE(strategy_.on_market_data(
      make_update(2, 1, mk::algo::Side::kAsk, 10000), signal_));
}

TEST_F(SpreadStrategyTest, EqualBidAskRejection) {
  // Bid=10000, Ask=10000 → locked (ask <= bid).
  EXPECT_FALSE(strategy_.on_market_data(
      make_update(1, 1, mk::algo::Side::kBid, 10000), signal_));
  EXPECT_FALSE(strategy_.on_market_data(
      make_update(2, 1, mk::algo::Side::kAsk, 10000), signal_));
}

// -- Multi-symbol --

TEST_F(SpreadStrategyTest, MultiSymbolIndependent) {
  // Symbol 1: narrow spread — no signal.
  EXPECT_FALSE(strategy_.on_market_data(
      make_update(1, 1, mk::algo::Side::kBid, 10000), signal_));
  EXPECT_FALSE(strategy_.on_market_data(
      make_update(2, 1, mk::algo::Side::kAsk, 10500), signal_));

  // Symbol 2: wide spread — signal.
  EXPECT_FALSE(strategy_.on_market_data(
      make_update(3, 2, mk::algo::Side::kBid, 20000), signal_));
  ASSERT_TRUE(strategy_.on_market_data(
      make_update(4, 2, mk::algo::Side::kAsk, 25000), signal_));
  EXPECT_EQ(signal_.symbol_id, 2U);

  // Symbol 1 still unaffected.
  EXPECT_EQ(strategy_.spread(1), 500);
}

// -- Observers --

TEST_F(SpreadStrategyTest, Observers) {
  EXPECT_FALSE(strategy_.on_market_data(
      make_update(1, 1, mk::algo::Side::kBid, 10000), signal_));
  // Spread 2000 > threshold 1000 → triggers signal.
  EXPECT_TRUE(strategy_.on_market_data(
      make_update(2, 1, mk::algo::Side::kAsk, 12000), signal_));

  EXPECT_EQ(strategy_.best_bid(1), 10000);
  EXPECT_EQ(strategy_.best_ask(1), 12000);
  EXPECT_EQ(strategy_.spread(1), 2000);
  EXPECT_EQ(strategy_.signals_generated(), 1U);

  // No data for symbol 2.
  EXPECT_EQ(strategy_.spread(2), 0);
}

} // namespace
