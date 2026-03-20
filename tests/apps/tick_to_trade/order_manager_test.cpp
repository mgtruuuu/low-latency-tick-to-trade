#include "tick_to_trade/order_manager.hpp"
#include "tick_to_trade/strategy_ctx.hpp"

#include "shared/protocol.hpp"
#include "tick_to_trade/spread_strategy.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace {

using OM = mk::app::OrderManager;

constexpr std::uint32_t kTestMaxSymbols = 2;
constexpr std::uint32_t kTestMaxOutstanding = 4;
using TestStrategy = mk::app::SpreadStrategy<kTestMaxSymbols>;

// Timeout test constants: 100ms timeout, 1ms tick → 100 ticks delay.
// wheel_size must be power-of-2 and > delay (256 > 100).
// max_timers = max_outstanding (one timer per order).
constexpr std::int64_t kTestTimeoutNs = 100'000'000; // 100ms
constexpr std::size_t kTestWheelSize = 256;
constexpr std::size_t kTestMaxTimers = 4;

mk::app::Signal make_signal(std::uint32_t symbol_id, mk::algo::Side side,
                             mk::algo::Price price, mk::algo::Qty qty = 10) {
  return {.side = side, .price = price, .qty = qty, .symbol_id = symbol_id};
}

class OrderManagerTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Construct OrderCtx from stack buffer, then OrderManager on top.
    // max_position=100, max_order_size=50,
    // max_notional=500'000, rate_limit=100/sec, timeout=100ms.
    ctx_ = mk::app::make_strategy_ctx<TestStrategy>(buf_.data(), 0, 0, 0,
                                                    kTestMaxOutstanding,
                                                    kTestMaxOutstanding,
                                                    kTestWheelSize,
                                                    kTestMaxTimers);
    om_ = std::construct_at(reinterpret_cast<OM *>(om_storage_),
                            ctx_,
                            /*max_position=*/100,
                            /*max_order_size=*/50,
                            /*max_notional=*/500'000,
                            /*max_orders_per_window=*/100,
                            /*rate_window_ns=*/1'000'000'000,
                            /*order_timeout_ns=*/kTestTimeoutNs);
  }

  void TearDown() override { std::destroy_at(om_); }

  // Send a signal and return the generated NewOrder. Asserts success.
  mk::app::NewOrder send_order(std::uint32_t symbol_id, mk::algo::Side side,
                                mk::algo::Price price, mk::algo::Qty qty = 10) {
    auto sig = make_signal(symbol_id, side, price, qty);
    mk::app::NewOrder order{};
    EXPECT_TRUE(om_->on_signal(sig, order));
    return order;
  }

  // Simulate an ack from the exchange for a given order.
  void ack_order(std::uint64_t client_order_id, std::uint64_t exch_id = 1000) {
    mk::app::OrderAck ack{};
    ack.client_order_id = client_order_id;
    ack.exchange_order_id = exch_id;
    ack.send_ts = 0;
    om_->on_order_ack(ack);
  }

  // Simulate a full fill from the exchange.
  void fill_order(std::uint64_t client_order_id, mk::algo::Qty fill_qty,
                  mk::algo::Qty remaining = 0, mk::algo::Price fill_price = 10000) {
    mk::app::FillReport fill{};
    fill.client_order_id = client_order_id;
    fill.exchange_order_id = 1000;
    fill.fill_price = fill_price;
    fill.fill_qty = fill_qty;
    fill.remaining_qty = remaining;
    fill.send_ts = 0;
    om_->on_fill(fill);
  }

  // TCP buffer sizes are zero — strategy thread TCP I/O is not exercised
  // in OrderManager tests. OMS state occupies the full buffer.
  std::array<std::byte,
             mk::app::strategy_ctx_buf_size<TestStrategy>(0, 0, 0,
                                                          kTestMaxOutstanding,
                                                          kTestWheelSize,
                                                          kTestMaxTimers)>
      buf_{};
  mk::app::StrategyCtx ctx_{};
  alignas(OM) std::byte om_storage_[sizeof(OM)]{};
  OM *om_{nullptr};
};

// -- Basic order generation --

TEST_F(OrderManagerTest, BasicOrderGeneration) {
  auto sig = make_signal(1, mk::algo::Side::kBid, 10000, 10);
  mk::app::NewOrder order{};
  ASSERT_TRUE(om_->on_signal(sig, order));

  EXPECT_EQ(order.client_order_id, 1U);
  EXPECT_EQ(order.symbol_id, 1U);
  EXPECT_EQ(order.side, mk::algo::Side::kBid);
  EXPECT_EQ(order.price, 10000);
  EXPECT_EQ(order.qty, 10U);
  EXPECT_EQ(om_->outstanding_count(), 1U);
  EXPECT_EQ(om_->orders_sent(), 1U);
}

// -- Risk checks --

TEST_F(OrderManagerTest, RiskRejectOutstanding) {
  // max_outstanding = 4. Fill up, then try one more.
  for (int i = 0; i < 4; ++i) {
    auto sig = make_signal(1, mk::algo::Side::kBid, 10000, 10);
    mk::app::NewOrder order{};
    ASSERT_TRUE(om_->on_signal(sig, order));
  }

  auto sig = make_signal(1, mk::algo::Side::kBid, 10000, 10);
  mk::app::NewOrder order{};
  EXPECT_FALSE(om_->on_signal(sig, order));
  EXPECT_EQ(om_->risk_rejects_outstanding(), 1U);
}

TEST_F(OrderManagerTest, RiskRejectMaxSize) {
  // max_order_size = 50. Try qty = 51.
  auto sig = make_signal(1, mk::algo::Side::kBid, 10000, 51);
  mk::app::NewOrder order{};
  EXPECT_FALSE(om_->on_signal(sig, order));
  EXPECT_EQ(om_->risk_rejects_size(), 1U);
}

TEST_F(OrderManagerTest, RiskRejectMaxNotional) {
  // max_notional = 500'000. price=100'000, qty=10 → notional=1'000'000.
  auto sig = make_signal(1, mk::algo::Side::kBid, 100'000, 10);
  mk::app::NewOrder order{};
  EXPECT_FALSE(om_->on_signal(sig, order));
  EXPECT_EQ(om_->risk_rejects_notional(), 1U);
}

TEST_F(OrderManagerTest, RiskRejectPosition) {
  // max_position = 100. Buy 50 and fill, then try to buy 60.
  auto order1 = send_order(1, mk::algo::Side::kBid, 10000, 50);
  fill_order(order1.client_order_id, 50, 0);
  EXPECT_EQ(om_->net_position(1), 50);

  auto order2 = send_order(1, mk::algo::Side::kBid, 10000, 50);
  fill_order(order2.client_order_id, 50, 0);
  EXPECT_EQ(om_->net_position(1), 100);

  // Position is 100, buying 10 more would be 110 > 100 limit.
  auto sig = make_signal(1, mk::algo::Side::kBid, 10000, 10);
  mk::app::NewOrder order{};
  EXPECT_FALSE(om_->on_signal(sig, order));
  EXPECT_EQ(om_->risk_rejects_position(), 1U);
}

// -- Fill handling --

TEST_F(OrderManagerTest, FillUpdatesPosition) {
  auto order = send_order(1, mk::algo::Side::kBid, 10000, 20);
  EXPECT_EQ(om_->outstanding_count(), 1U);

  fill_order(order.client_order_id, 20, 0);

  EXPECT_EQ(om_->net_position(1), 20);
  EXPECT_EQ(om_->outstanding_count(), 0U);
  EXPECT_EQ(om_->fills_received(), 1U);
  EXPECT_EQ(om_->total_fill_qty(), 20U);
}

TEST_F(OrderManagerTest, PartialFill) {
  auto order = send_order(1, mk::algo::Side::kBid, 10000, 20);

  // Partial fill: 10 of 20, remaining = 10.
  fill_order(order.client_order_id, 10, 10);

  EXPECT_EQ(om_->net_position(1), 10);
  EXPECT_EQ(om_->outstanding_count(), 1U); // Still outstanding.
  EXPECT_EQ(om_->fills_received(), 1U);
}

TEST_F(OrderManagerTest, SellFillDecreasesPosition) {
  // Buy and fill to get position.
  auto buy = send_order(1, mk::algo::Side::kBid, 10000, 20);
  fill_order(buy.client_order_id, 20, 0);
  EXPECT_EQ(om_->net_position(1), 20);

  // Sell and fill.
  auto sell = send_order(1, mk::algo::Side::kAsk, 10000, 10);
  fill_order(sell.client_order_id, 10, 0);
  EXPECT_EQ(om_->net_position(1), 10);
}

// -- Order reject --

TEST_F(OrderManagerTest, OrderRejectCleansUp) {
  auto order = send_order(1, mk::algo::Side::kBid, 10000, 10);
  EXPECT_EQ(om_->outstanding_count(), 1U);

  mk::app::OrderReject reject{};
  reject.client_order_id = order.client_order_id;
  reject.reason = mk::app::RejectReason::kUnknown;
  reject.send_ts = 0;
  om_->on_order_reject(reject);

  EXPECT_EQ(om_->outstanding_count(), 0U);
  EXPECT_EQ(om_->rejects_received(), 1U);
}

// -- Kill switch --

TEST_F(OrderManagerTest, KillSwitchBlocksOrders) {
  auto batch = om_->trigger_kill_switch();
  EXPECT_EQ(batch.count, 0U);
  EXPECT_EQ(om_->kill_switch_state(), OM::KillSwitchState::kComplete);

  auto sig = make_signal(1, mk::algo::Side::kBid, 10000, 10);
  mk::app::NewOrder order{};
  EXPECT_FALSE(om_->on_signal(sig, order));
  EXPECT_EQ(om_->risk_rejects_killswitch(), 1U);
}

TEST_F(OrderManagerTest, KillSwitchCancelBatch) {
  send_order(1, mk::algo::Side::kBid, 10000, 10);
  send_order(1, mk::algo::Side::kAsk, 11000, 10);
  send_order(2, mk::algo::Side::kBid, 20000, 10);
  EXPECT_EQ(om_->outstanding_count(), 3U);

  auto batch = om_->trigger_kill_switch();
  EXPECT_EQ(batch.count, 3U);
  EXPECT_EQ(om_->kill_switch_state(), OM::KillSwitchState::kDraining);
}

TEST_F(OrderManagerTest, KillSwitchDrainComplete) {
  auto order1 = send_order(1, mk::algo::Side::kBid, 10000, 10);
  auto order2 = send_order(1, mk::algo::Side::kAsk, 11000, 10);

  auto batch = om_->trigger_kill_switch();
  ASSERT_EQ(batch.count, 2U);
  EXPECT_EQ(om_->kill_switch_state(), OM::KillSwitchState::kDraining);

  // First cancel ack — still draining.
  mk::app::CancelAck ack1{};
  ack1.client_order_id = order1.client_order_id;
  ack1.send_ts = 0;
  om_->on_cancel_ack(ack1);
  EXPECT_EQ(om_->kill_switch_state(), OM::KillSwitchState::kDraining);

  // Second cancel ack — complete.
  mk::app::CancelAck ack2{};
  ack2.client_order_id = order2.client_order_id;
  ack2.send_ts = 0;
  om_->on_cancel_ack(ack2);
  EXPECT_EQ(om_->kill_switch_state(), OM::KillSwitchState::kComplete);
}

// -- Modify logic --

TEST_F(OrderManagerTest, ModifyRestingOrder) {
  // Place an order — it becomes the active resting order for (sym=1, side=bid).
  auto order = send_order(1, mk::algo::Side::kBid, 10000, 10);
  ack_order(order.client_order_id);

  // New signal for same (sym=1, side=bid) but different price.
  auto sig = make_signal(1, mk::algo::Side::kBid, 10500, 10);
  mk::app::ModifyOrder modify{};
  ASSERT_TRUE(om_->check_modify(sig, modify));

  EXPECT_EQ(modify.client_order_id, order.client_order_id);
  EXPECT_EQ(modify.symbol_id, 1U);
  EXPECT_EQ(modify.new_price, 10500);
  EXPECT_EQ(modify.new_qty, 10U);
  EXPECT_EQ(om_->modifies_sent(), 1U);
}

TEST_F(OrderManagerTest, ModifySamePriceSkipped) {
  auto order = send_order(1, mk::algo::Side::kBid, 10000, 10);
  ack_order(order.client_order_id);

  // Same price — no modify needed.
  auto sig = make_signal(1, mk::algo::Side::kBid, 10000, 10);
  mk::app::ModifyOrder modify{};
  EXPECT_FALSE(om_->check_modify(sig, modify));
}

TEST_F(OrderManagerTest, NoModifyWithoutResting) {
  // No orders placed — nothing to modify.
  auto sig = make_signal(1, mk::algo::Side::kBid, 10000, 10);
  mk::app::ModifyOrder modify{};
  EXPECT_FALSE(om_->check_modify(sig, modify));
}

// -- Order timeout --

TEST_F(OrderManagerTest, TimeoutFiresCancelForUnackedOrder) {
  auto order = send_order(1, mk::algo::Side::kBid, 10000, 10);
  EXPECT_EQ(om_->outstanding_count(), 1U);

  // Advance past the 100ms timeout.
  auto batch = om_->advance_timeouts(kTestTimeoutNs + 1'000'000);
  EXPECT_EQ(batch.count, 1U);
  EXPECT_EQ(batch.cancels[0].client_order_id, order.client_order_id);
  EXPECT_EQ(batch.cancels[0].symbol_id, 1U);
  EXPECT_EQ(om_->outstanding_count(), 0U);
  EXPECT_EQ(om_->timeouts_fired(), 1U);
}

TEST_F(OrderManagerTest, TimeoutNotFiredBeforeDeadline) {
  send_order(1, mk::algo::Side::kBid, 10000, 10);

  // Advance to 50ms — half the timeout. Should not fire.
  auto batch = om_->advance_timeouts(50'000'000);
  EXPECT_EQ(batch.count, 0U);
  EXPECT_EQ(om_->outstanding_count(), 1U);
  EXPECT_EQ(om_->timeouts_fired(), 0U);
}

TEST_F(OrderManagerTest, TimeoutCancelledOnFill) {
  auto order = send_order(1, mk::algo::Side::kBid, 10000, 10);
  fill_order(order.client_order_id, 10, 0);
  EXPECT_EQ(om_->outstanding_count(), 0U);

  // Advance past timeout — should produce no cancel (timer was cancelled).
  auto batch = om_->advance_timeouts(kTestTimeoutNs + 1'000'000);
  EXPECT_EQ(batch.count, 0U);
  EXPECT_EQ(om_->timeouts_fired(), 0U);
}

TEST_F(OrderManagerTest, TimeoutCancelledOnReject) {
  auto order = send_order(1, mk::algo::Side::kBid, 10000, 10);

  mk::app::OrderReject reject{};
  reject.client_order_id = order.client_order_id;
  reject.reason = mk::app::RejectReason::kUnknown;
  reject.send_ts = 0;
  om_->on_order_reject(reject);
  EXPECT_EQ(om_->outstanding_count(), 0U);

  // Advance past timeout — should produce no cancel.
  auto batch = om_->advance_timeouts(kTestTimeoutNs + 1'000'000);
  EXPECT_EQ(batch.count, 0U);
  EXPECT_EQ(om_->timeouts_fired(), 0U);
}

TEST_F(OrderManagerTest, NoTimeoutDuringKillSwitch) {
  send_order(1, mk::algo::Side::kBid, 10000, 10);

  auto ks_batch = om_->trigger_kill_switch();
  EXPECT_EQ(ks_batch.count, 1U);

  // Kill switch resets the wheel. Advance past timeout — should be empty.
  auto batch = om_->advance_timeouts(kTestTimeoutNs + 1'000'000);
  EXPECT_EQ(batch.count, 0U);
  EXPECT_EQ(om_->timeouts_fired(), 0U);
}

TEST_F(OrderManagerTest, TimeoutCancelledOnCancelAck) {
  auto order = send_order(1, mk::algo::Side::kBid, 10000, 10);

  mk::app::CancelAck ack{};
  ack.client_order_id = order.client_order_id;
  ack.send_ts = 0;
  om_->on_cancel_ack(ack);
  EXPECT_EQ(om_->outstanding_count(), 0U);

  // Advance past timeout — should produce no cancel.
  auto batch = om_->advance_timeouts(kTestTimeoutNs + 1'000'000);
  EXPECT_EQ(batch.count, 0U);
  EXPECT_EQ(om_->timeouts_fired(), 0U);
}

TEST_F(OrderManagerTest, MultipleOrdersTimeout) {
  // Send 3 orders, advance past timeout — all 3 should be cancelled.
  auto o1 = send_order(1, mk::algo::Side::kBid, 10000, 10);
  auto o2 = send_order(1, mk::algo::Side::kAsk, 11000, 10);
  auto o3 = send_order(2, mk::algo::Side::kBid, 20000, 10);
  EXPECT_EQ(om_->outstanding_count(), 3U);

  auto batch = om_->advance_timeouts(kTestTimeoutNs + 1'000'000);
  EXPECT_EQ(batch.count, 3U);
  EXPECT_EQ(om_->outstanding_count(), 0U);
  EXPECT_EQ(om_->timeouts_fired(), 3U);

  // Verify all 3 client_order_ids are present (order may vary).
  std::array<std::uint64_t, 3> ids{};
  for (std::uint32_t i = 0; i < batch.count; ++i) {
    ids[i] = batch.cancels[i].client_order_id;
  }
  std::ranges::sort(ids);
  EXPECT_EQ(ids[0], o1.client_order_id);
  EXPECT_EQ(ids[1], o2.client_order_id);
  EXPECT_EQ(ids[2], o3.client_order_id);
}

TEST_F(OrderManagerTest, PartialFillDoesNotCancelTimeout) {
  auto order = send_order(1, mk::algo::Side::kBid, 10000, 20);

  // Partial fill: 10 of 20, remaining = 10. Timer should stay active.
  fill_order(order.client_order_id, 10, 10);
  EXPECT_EQ(om_->outstanding_count(), 1U);

  // Advance past timeout — should fire (partial fill doesn't cancel timer).
  auto batch = om_->advance_timeouts(kTestTimeoutNs + 1'000'000);
  EXPECT_EQ(batch.count, 1U);
  EXPECT_EQ(batch.cancels[0].client_order_id, order.client_order_id);
  EXPECT_EQ(om_->timeouts_fired(), 1U);
}

TEST_F(OrderManagerTest, IncrementalAdvanceFiresAtCorrectTime) {
  send_order(1, mk::algo::Side::kBid, 10000, 10);

  // Advance 50ms — no fire.
  auto b1 = om_->advance_timeouts(50'000'000);
  EXPECT_EQ(b1.count, 0U);

  // Advance another 30ms (total 80ms) — still no fire.
  auto b2 = om_->advance_timeouts(80'000'000);
  EXPECT_EQ(b2.count, 0U);

  // Advance another 21ms (total 101ms) — fires.
  auto b3 = om_->advance_timeouts(101'000'000);
  EXPECT_EQ(b3.count, 1U);
  EXPECT_EQ(om_->timeouts_fired(), 1U);
}

// -- Cancel reject handling --

TEST_F(OrderManagerTest, CancelRejectCleansUpOutstanding) {
  auto order = send_order(1, mk::algo::Side::kBid, 10000, 10);
  EXPECT_EQ(om_->outstanding_count(), 1U);

  mk::app::CancelReject cr{};
  cr.client_order_id = order.client_order_id;
  cr.reason = mk::app::RejectReason::kUnknown;
  cr.send_ts = 0;
  om_->on_cancel_reject(cr);

  EXPECT_EQ(om_->outstanding_count(), 0U);
}

TEST_F(OrderManagerTest, CancelRejectForUnknownOrderDoesNotCrash) {
  // No orders placed. This must be a no-op.
  mk::app::CancelReject cr{};
  cr.client_order_id = 99999;
  cr.reason = mk::app::RejectReason::kUnknown;
  cr.send_ts = 0;
  om_->on_cancel_reject(cr);

  EXPECT_EQ(om_->outstanding_count(), 0U);
}

TEST_F(OrderManagerTest, KillSwitchDrainViaCancelReject) {
  auto order1 = send_order(1, mk::algo::Side::kBid, 10000, 10);
  auto order2 = send_order(1, mk::algo::Side::kAsk, 11000, 10);

  auto batch = om_->trigger_kill_switch();
  ASSERT_EQ(batch.count, 2U);
  EXPECT_EQ(om_->kill_switch_state(), OM::KillSwitchState::kDraining);

  // First cancel ack.
  mk::app::CancelAck ack{};
  ack.client_order_id = order1.client_order_id;
  ack.send_ts = 0;
  om_->on_cancel_ack(ack);
  EXPECT_EQ(om_->kill_switch_state(), OM::KillSwitchState::kDraining);

  // Second response is a cancel reject — should still drain to kComplete.
  mk::app::CancelReject cr{};
  cr.client_order_id = order2.client_order_id;
  cr.reason = mk::app::RejectReason::kUnknown;
  cr.send_ts = 0;
  om_->on_cancel_reject(cr);
  EXPECT_EQ(om_->kill_switch_state(), OM::KillSwitchState::kComplete);
}

// -- Modify ack / reject handling --

TEST_F(OrderManagerTest, ModifyAckIncrementsCounter) {
  auto order = send_order(1, mk::algo::Side::kBid, 10000, 10);
  ack_order(order.client_order_id);

  auto sig = make_signal(1, mk::algo::Side::kBid, 10500, 10);
  mk::app::ModifyOrder modify{};
  ASSERT_TRUE(om_->check_modify(sig, modify));
  EXPECT_EQ(om_->modifies_sent(), 1U);

  mk::app::ModifyAck mack{};
  mack.client_order_id = modify.client_order_id;
  mack.new_exchange_order_id = 2000;
  mack.send_ts = 0;
  om_->on_modify_ack(mack);

  EXPECT_EQ(om_->modifies_acked(), 1U);
  EXPECT_EQ(om_->outstanding_count(), 1U); // order still outstanding
}

TEST_F(OrderManagerTest, ModifyRejectClearsActiveAndAllowsNewOrder) {
  auto order = send_order(1, mk::algo::Side::kBid, 10000, 10);
  ack_order(order.client_order_id);

  // Modify to new price.
  auto sig = make_signal(1, mk::algo::Side::kBid, 10500, 10);
  mk::app::ModifyOrder modify{};
  ASSERT_TRUE(om_->check_modify(sig, modify));

  // Reject the modify.
  mk::app::ModifyReject mrej{};
  mrej.client_order_id = modify.client_order_id;
  mrej.reason = mk::app::RejectReason::kUnknown;
  mrej.send_ts = 0;
  om_->on_modify_reject(mrej);

  // Active tracking cleared — check_modify should now return false
  // (no resting order to modify).
  auto sig2 = make_signal(1, mk::algo::Side::kBid, 11000, 10);
  mk::app::ModifyOrder modify2{};
  EXPECT_FALSE(om_->check_modify(sig2, modify2));

  // But outstanding is still tracked (order may still exist in exchange).
  EXPECT_EQ(om_->outstanding_count(), 1U);
}

TEST_F(OrderManagerTest, ModifyRejectForUnknownOrderDoesNotCrash) {
  mk::app::ModifyReject mrej{};
  mrej.client_order_id = 99999;
  mrej.reason = mk::app::RejectReason::kUnknown;
  mrej.send_ts = 0;
  om_->on_modify_reject(mrej);

  EXPECT_EQ(om_->outstanding_count(), 0U);
}

// -- Rate limit --

TEST_F(OrderManagerTest, RateLimitRejectAfterTokenExhaustion) {
  // Token bucket starts at 100. Send 4 orders at a time (max_outstanding=4),
  // fill them, repeat until bucket is exhausted. Runs in <10ms so no refill.
  // Alternate buy/sell each cycle to keep net_position bounded.
  for (int cycle = 0; cycle < 25; ++cycle) {
    const auto side = (cycle % 2 == 0) ? mk::algo::Side::kBid
                                       : mk::algo::Side::kAsk;
    // Send 4 orders.
    std::array<std::uint64_t, 4> ids{};
    for (int i = 0; i < 4; ++i) {
      auto sig = make_signal(1, side, 10000, 10);
      mk::app::NewOrder order{};
      ASSERT_TRUE(om_->on_signal(sig, order))
          << "cycle=" << cycle << " i=" << i;
      ids[static_cast<std::size_t>(i)] = order.client_order_id;
    }
    // Fill all 4.
    for (auto id : ids) {
      fill_order(id, 10, 0);
    }
    ASSERT_EQ(om_->outstanding_count(), 0U);
  }
  // 100 tokens consumed. Next signal should be rate-limited.
  auto sig = make_signal(1, mk::algo::Side::kBid, 10000, 10);
  mk::app::NewOrder order{};
  EXPECT_FALSE(om_->on_signal(sig, order));
  EXPECT_GE(om_->risk_rejects_rate(), 1U);
}

} // namespace
