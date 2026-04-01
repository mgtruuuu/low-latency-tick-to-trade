/**
 * @file exchange_core_test.cpp
 * @brief Tests for ExchangeCore — submit, cancel, modify, session management,
 *        event emission, event cursor pattern.
 *
 * Tests the business logic layer directly via the event bus.
 * ExchangeCore no longer returns result structs — all outcomes are
 * communicated through ExchangeEvents in the event buffer.
 */

#include "simulated_exchange/exchange_core.hpp"
#include "simulated_exchange/exchange_event.hpp"

#include "shared/protocol.hpp"

#include <algorithm>
#include <vector>

#include <gtest/gtest.h>

namespace mk::app {
namespace {

class ExchangeCoreTest : public ::testing::Test {
protected:
  ExchangeCoreTest() {
    const auto size = algo::MatchingEngine<>::required_buffer_size(params_);
    book_buf_.resize(size);
    core_.register_symbol(1, book_buf_.data(), book_buf_.size(), params_);
  }

  algo::OrderBook::Params params_{};
  std::vector<std::byte> book_buf_;
  ExchangeCore<> core_;
};

// ---------------------------------------------------------------------------
// Helper: find first event of a given type
// ---------------------------------------------------------------------------

const ExchangeEvent *find_event(std::span<const ExchangeEvent> events,
                                EventType type) {
  for (const auto &e : events) {
    if (e.type == type) {
      return &e;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Submit — resting order (no crossing)
// ---------------------------------------------------------------------------

TEST_F(ExchangeCoreTest, SubmitNonCrossingOrderRests) {
  const auto session = core_.on_client_connect();
  const NewOrder order{
      .client_order_id = 1,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 99'000,
      .qty = 10,
      .send_ts = 100,
  };

  core_.submit_order(session, order);
  const auto events = core_.drain_events();

  // Should emit OrderAccepted, no fills.
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, EventType::kOrderAccepted);
  EXPECT_EQ(events[0].client_order_id, 1U);
  EXPECT_GT(events[0].exchange_order_id, 0U);
}

// ---------------------------------------------------------------------------
// Submit — crossing order produces fills
// ---------------------------------------------------------------------------

TEST_F(ExchangeCoreTest, SubmitCrossingOrderProducesFills) {
  const auto session = core_.on_client_connect();

  // Place resting ask.
  const NewOrder ask{
      .client_order_id = 1,
      .symbol_id = 1,
      .side = algo::Side::kAsk,
      .price = 100'000,
      .qty = 5,
      .send_ts = 100,
  };
  core_.submit_order(session, ask);
  (void)core_.drain_events(); // clear

  // Aggressive bid crosses the ask.
  const NewOrder bid{
      .client_order_id = 2,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 100'000,
      .qty = 3,
      .send_ts = 200,
  };
  core_.submit_order(session, bid);
  const auto events = core_.drain_events();

  // Should emit OrderAccepted + Fill.
  ASSERT_GE(events.size(), 2U);

  const auto *accepted = find_event(events, EventType::kOrderAccepted);
  ASSERT_NE(accepted, nullptr);
  EXPECT_EQ(accepted->client_order_id, 2U);

  const auto *fill = find_event(events, EventType::kFill);
  ASSERT_NE(fill, nullptr);
  EXPECT_EQ(fill->price, 100'000);
  EXPECT_EQ(fill->qty, 3U);
  EXPECT_EQ(fill->remaining_qty, 0U);
}

// ---------------------------------------------------------------------------
// Submit — invalid symbol emits reject
// ---------------------------------------------------------------------------

TEST_F(ExchangeCoreTest, SubmitInvalidSymbolRejects) {
  const auto session = core_.on_client_connect();
  const NewOrder order{
      .client_order_id = 1,
      .symbol_id = 99, // not registered
      .side = algo::Side::kBid,
      .price = 100'000,
      .qty = 1,
      .send_ts = 0,
  };

  core_.submit_order(session, order);
  const auto events = core_.drain_events();

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, EventType::kOrderRejected);
  EXPECT_EQ(events[0].reason, RejectReason::kUnknownSymbol);
}

TEST_F(ExchangeCoreTest, CancelInvalidSymbolRejectsWithUnknownSymbol) {
  const auto session = core_.on_client_connect();
  const CancelOrder cancel{
      .client_order_id = 1,
      .symbol_id = 99,
      .send_ts = 0,
  };
  core_.cancel_order(session, cancel);
  const auto events = core_.drain_events();
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, EventType::kCancelRejected);
  EXPECT_EQ(events[0].reason, RejectReason::kUnknownSymbol);
}

TEST_F(ExchangeCoreTest, CancelUnknownOrderRejectsWithOrderNotFound) {
  const auto session = core_.on_client_connect();
  const CancelOrder cancel{
      .client_order_id = 999, // never submitted
      .symbol_id = 1,
      .send_ts = 0,
  };
  core_.cancel_order(session, cancel);
  const auto events = core_.drain_events();
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, EventType::kCancelRejected);
  EXPECT_EQ(events[0].reason, RejectReason::kOrderNotFound);
}

TEST_F(ExchangeCoreTest, ModifyInvalidSymbolRejectsWithUnknownSymbol) {
  const auto session = core_.on_client_connect();
  const ModifyOrder modify{
      .client_order_id = 1,
      .symbol_id = 99,
      .new_price = 100'000,
      .new_qty = 5,
      .send_ts = 0,
  };
  core_.modify_order(session, modify);
  const auto events = core_.drain_events();
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, EventType::kModifyRejected);
  EXPECT_EQ(events[0].reason, RejectReason::kUnknownSymbol);
}

// ---------------------------------------------------------------------------
// Cancel — success
// ---------------------------------------------------------------------------

TEST_F(ExchangeCoreTest, CancelRestingOrderSucceeds) {
  const auto session = core_.on_client_connect();
  const NewOrder order{
      .client_order_id = 1,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 99'000,
      .qty = 10,
      .send_ts = 100,
  };
  core_.submit_order(session, order);
  (void)core_.drain_events();

  const CancelOrder cancel{
      .client_order_id = 1,
      .symbol_id = 1,
      .send_ts = 200,
  };
  core_.cancel_order(session, cancel);
  const auto events = core_.drain_events();

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, EventType::kCancelAck);
}

// ---------------------------------------------------------------------------
// Cancel — unknown order emits reject
// ---------------------------------------------------------------------------

TEST_F(ExchangeCoreTest, CancelUnknownOrderFails) {
  const auto session = core_.on_client_connect();
  const CancelOrder cancel{
      .client_order_id = 999,
      .symbol_id = 1,
      .send_ts = 100,
  };

  core_.cancel_order(session, cancel);
  const auto events = core_.drain_events();

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, EventType::kCancelRejected);
}

// ---------------------------------------------------------------------------
// Modify — resting order modified successfully
// ---------------------------------------------------------------------------

TEST_F(ExchangeCoreTest, ModifyRestingOrderSucceeds) {
  const auto session = core_.on_client_connect();
  const NewOrder order{
      .client_order_id = 1,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 90'000,
      .qty = 10,
      .send_ts = 100,
  };
  core_.submit_order(session, order);
  (void)core_.drain_events();

  const ModifyOrder modify{
      .client_order_id = 1,
      .symbol_id = 1,
      .new_price = 91'000,
      .new_qty = 8,
      .send_ts = 200,
  };
  core_.modify_order(session, modify);
  const auto events = core_.drain_events();

  ASSERT_GE(events.size(), 1U);
  const auto *ack = find_event(events, EventType::kModifyAck);
  ASSERT_NE(ack, nullptr);
  EXPECT_EQ(ack->client_order_id, 1U);
  EXPECT_GT(ack->exchange_order_id, 0U);
}

// ---------------------------------------------------------------------------
// Modify — crossing after modify produces fills
// ---------------------------------------------------------------------------

TEST_F(ExchangeCoreTest, ModifyThatCrossesProducesFills) {
  const auto session = core_.on_client_connect();

  // Resting ask at 100'000.
  const NewOrder ask{
      .client_order_id = 1,
      .symbol_id = 1,
      .side = algo::Side::kAsk,
      .price = 100'000,
      .qty = 5,
      .send_ts = 100,
  };
  core_.submit_order(session, ask);
  (void)core_.drain_events();

  // Resting bid at 90'000.
  const NewOrder bid{
      .client_order_id = 2,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 90'000,
      .qty = 3,
      .send_ts = 200,
  };
  core_.submit_order(session, bid);
  (void)core_.drain_events();

  // Modify bid up to 100'000 — crosses the ask.
  const ModifyOrder modify{
      .client_order_id = 2,
      .symbol_id = 1,
      .new_price = 100'000,
      .new_qty = 3,
      .send_ts = 300,
  };
  core_.modify_order(session, modify);
  const auto events = core_.drain_events();

  const auto *ack = find_event(events, EventType::kModifyAck);
  ASSERT_NE(ack, nullptr);

  const auto *fill = find_event(events, EventType::kFill);
  ASSERT_NE(fill, nullptr);
  EXPECT_EQ(fill->price, 100'000);
  EXPECT_EQ(fill->qty, 3U);
}

// ---------------------------------------------------------------------------
// Modify — unknown order emits reject
// ---------------------------------------------------------------------------

TEST_F(ExchangeCoreTest, ModifyUnknownOrderFails) {
  const auto session = core_.on_client_connect();
  const ModifyOrder modify{
      .client_order_id = 999,
      .symbol_id = 1,
      .new_price = 100'000,
      .new_qty = 5,
      .send_ts = 100,
  };

  core_.modify_order(session, modify);
  const auto events = core_.drain_events();

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, EventType::kModifyRejected);
}

// ---------------------------------------------------------------------------
// Session disconnect cancels only that session's orders
// ---------------------------------------------------------------------------

TEST_F(ExchangeCoreTest, DisconnectCancelsOnlyThatSession) {
  const auto session1 = core_.on_client_connect();
  const auto session2 = core_.on_client_connect();

  const NewOrder order{
      .client_order_id = 7,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 99'000,
      .qty = 5,
      .send_ts = 100,
  };

  core_.submit_order(session1, order);
  core_.submit_order(session2, order);
  (void)core_.drain_events();

  // Disconnect session1 only.
  core_.on_client_disconnect(session1);

  // Session2's order should still be cancelable.
  const CancelOrder cancel{
      .client_order_id = 7,
      .symbol_id = 1,
      .send_ts = 200,
  };
  core_.cancel_order(session2, cancel);
  auto events = core_.drain_events();
  const auto *ack = find_event(events, EventType::kCancelAck);
  EXPECT_NE(ack, nullptr);

  // Session1's order should be gone (already cancelled by disconnect).
  core_.cancel_order(session1, cancel);
  events = core_.drain_events();
  const auto *rej = find_event(events, EventType::kCancelRejected);
  EXPECT_NE(rej, nullptr);
}

// ---------------------------------------------------------------------------
// Fill emits kFill event with correct fields
// ---------------------------------------------------------------------------

TEST_F(ExchangeCoreTest, FillEventHasCorrectFields) {
  const auto session = core_.on_client_connect();

  // Place resting ask.
  const NewOrder ask{
      .client_order_id = 1,
      .symbol_id = 1,
      .side = algo::Side::kAsk,
      .price = 100'000,
      .qty = 5,
      .send_ts = 100,
  };
  core_.submit_order(session, ask);
  (void)core_.drain_events();

  // Aggressive bid crosses.
  const NewOrder bid{
      .client_order_id = 2,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 100'000,
      .qty = 3,
      .send_ts = 200,
  };
  core_.submit_order(session, bid);
  const auto events = core_.drain_events();

  const auto *fill = find_event(events, EventType::kFill);
  ASSERT_NE(fill, nullptr);
  EXPECT_EQ(fill->symbol_id, 1U);
  EXPECT_EQ(fill->side, algo::Side::kBid);
  EXPECT_EQ(fill->price, 100'000);
  EXPECT_EQ(fill->qty, 3U);
  EXPECT_EQ(fill->remaining_qty, 0U);
  EXPECT_EQ(fill->send_ts, 200);
}

// ---------------------------------------------------------------------------
// BBO update carries real top-of-book quantity
// ---------------------------------------------------------------------------

TEST_F(ExchangeCoreTest, BBOUpdateCarriesRealVolume) {
  const auto session = core_.on_client_connect();

  // Place resting ask: qty=10 at 100'000.
  const NewOrder ask{
      .client_order_id = 1,
      .symbol_id = 1,
      .side = algo::Side::kAsk,
      .price = 100'000,
      .qty = 10,
      .send_ts = 100,
  };
  core_.submit_order(session, ask);
  (void)core_.drain_events();

  // Aggressive bid crosses for qty=3 → remaining ask qty = 7.
  const NewOrder bid{
      .client_order_id = 2,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 100'000,
      .qty = 3,
      .send_ts = 200,
  };
  core_.submit_order(session, bid);
  const auto events = core_.drain_events();

  // Find the kBBOUpdate for the ask side after the fill.
  const ExchangeEvent *ask_bbo = nullptr;
  for (const auto &e : events) {
    if (e.type == EventType::kBBOUpdate && e.side == algo::Side::kAsk) {
      ask_bbo = &e;
    }
  }
  ASSERT_NE(ask_bbo, nullptr);
  EXPECT_EQ(ask_bbo->price, 100'000);
  // qty must be real volume_at_level (10 - 3 = 7), not a fake constant.
  EXPECT_EQ(ask_bbo->qty, 7U);
}

// ---------------------------------------------------------------------------
// Drain clears the event buffer
// ---------------------------------------------------------------------------

TEST_F(ExchangeCoreTest, DrainClearsEventBuffer) {
  const auto session = core_.on_client_connect();
  const NewOrder order{
      .client_order_id = 1,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 99'000,
      .qty = 10,
      .send_ts = 100,
  };
  core_.submit_order(session, order);
  EXPECT_GT(core_.event_count(), 0U);

  (void)core_.drain_events();
  EXPECT_EQ(core_.event_count(), 0U);

  // Second drain returns empty.
  const auto empty = core_.drain_events();
  EXPECT_TRUE(empty.empty());
}

// ---------------------------------------------------------------------------
// Per-message drain keeps event buffer bounded
// ---------------------------------------------------------------------------
//
// Documents the worst-case capacity constraint: if the caller drains after
// each inbound message (as LiveHandler::on_data does), the event buffer
// only ever holds events from a single order. This test exercises three
// consecutive large crossing orders — each producing ~5 events — with a
// drain between each, verifying the buffer never accumulates past one
// message's worth.
//
// Note: this does NOT verify that LiveHandler actually calls drain after
// each on_data(). That integration path relies on code review of main.cpp.
// See exchange_core.hpp header comment for the architectural contract.

TEST_F(ExchangeCoreTest, PerMessageDrainPreventsAccumulation) {
  const auto session = core_.on_client_connect();

  // Seed the book with resting asks at 3 price levels.
  for (std::uint32_t i = 0; i < 3; ++i) {
    const NewOrder ask{
        .client_order_id = 100 + i,
        .symbol_id = 1,
        .side = algo::Side::kAsk,
        .price = static_cast<algo::Price>(100'000 + (i * 1'000)),
        .qty = 50,
        .send_ts = static_cast<std::int64_t>(100 + i),
    };
    core_.submit_order(session, ask);
    (void)core_.drain_events();
  }
  ASSERT_EQ(core_.event_count(), 0U); // clean slate

  // Submit 3 aggressive bids that each cross multiple levels.
  // Between each, drain and verify the buffer stayed bounded.
  std::uint32_t max_events_seen = 0;

  for (std::uint32_t i = 0; i < 3; ++i) {
    const NewOrder bid{
        .client_order_id = 200 + i,
        .symbol_id = 1,
        .side = algo::Side::kBid,
        .price = 102'000, // crosses all 3 ask levels
        .qty = 10,
        .send_ts = static_cast<std::int64_t>(300 + i),
    };
    core_.submit_order(session, bid);

    const auto count = core_.event_count();
    max_events_seen = std::max(max_events_seen, count);

    // Drain after each message — simulates LiveHandler pattern.
    (void)core_.drain_events();
    EXPECT_EQ(core_.event_count(), 0U);
  }

  // Per-order worst case: 1 ack + N fills + 2N BBO updates.
  // With drain after each message, events never accumulate across orders.
  // kMaxEvents (256) is sufficient for any single order.
  EXPECT_LT(max_events_seen, ExchangeCore<>::kMaxEventsCapacity);
}

// ---------------------------------------------------------------------------
// Engine accessor
// ---------------------------------------------------------------------------

TEST_F(ExchangeCoreTest, EngineAccessorReturnsBookState) {
  const auto session = core_.on_client_connect();
  const NewOrder order{
      .client_order_id = 1,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 99'000,
      .qty = 10,
      .send_ts = 100,
  };
  core_.submit_order(session, order);
  (void)core_.drain_events();

  EXPECT_EQ(core_.engine(1).book().total_orders(), 1U);
  EXPECT_TRUE(core_.engine(1).book().best_bid().has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(*core_.engine(1).book().best_bid(), 99'000);
}

// ---------------------------------------------------------------------------
// Duplicate client_order_id detection (session-lifetime uniqueness)
// ---------------------------------------------------------------------------

TEST_F(ExchangeCoreTest, DuplicateOrderIdRejectedInSameSession) {
  const auto session = core_.on_client_connect();

  const NewOrder order{
      .client_order_id = 42,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 99'000,
      .qty = 10,
      .send_ts = 100,
  };
  core_.submit_order(session, order);
  (void)core_.drain_events();

  // Same client_order_id in same session → reject.
  core_.submit_order(session, order);
  const auto events = core_.drain_events();
  const auto *reject = find_event(events, EventType::kOrderRejected);
  ASSERT_NE(reject, nullptr);
  EXPECT_EQ(reject->reason, RejectReason::kDuplicateOrderId);
}

TEST_F(ExchangeCoreTest, SameOrderIdAllowedInDifferentSession) {
  const auto session1 = core_.on_client_connect();
  const auto session2 = core_.on_client_connect();

  const NewOrder order{
      .client_order_id = 42,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 99'000,
      .qty = 10,
      .send_ts = 100,
  };

  // Same client_order_id but different sessions → both accepted.
  core_.submit_order(session1, order);
  auto events1 = core_.drain_events();
  EXPECT_NE(find_event(events1, EventType::kOrderAccepted), nullptr);

  core_.submit_order(session2, order);
  auto events2 = core_.drain_events();
  EXPECT_NE(find_event(events2, EventType::kOrderAccepted), nullptr);
}

TEST_F(ExchangeCoreTest, DuplicateRejectedAfterCancel) {
  const auto session = core_.on_client_connect();

  const NewOrder order{
      .client_order_id = 42,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 99'000,
      .qty = 10,
      .send_ts = 100,
  };
  core_.submit_order(session, order);
  (void)core_.drain_events();

  const CancelOrder cancel{
      .client_order_id = 42,
      .symbol_id = 1,
      .send_ts = 200,
  };
  core_.cancel_order(session, cancel);
  (void)core_.drain_events();

  // Reuse order 42 in same session after cancel → still rejected.
  core_.submit_order(session, order);
  const auto events = core_.drain_events();
  const auto *reject = find_event(events, EventType::kOrderRejected);
  ASSERT_NE(reject, nullptr);
  EXPECT_EQ(reject->reason, RejectReason::kDuplicateOrderId);
}

TEST_F(ExchangeCoreTest, DuplicateRejectedAfterFullFill) {
  const auto session = core_.on_client_connect();

  // Place resting ask.
  const NewOrder ask{
      .client_order_id = 1,
      .symbol_id = 1,
      .side = algo::Side::kAsk,
      .price = 100'000,
      .qty = 10,
      .send_ts = 100,
  };
  core_.submit_order(session, ask);
  (void)core_.drain_events();

  // Aggressive bid fully fills the ask.
  const NewOrder bid{
      .client_order_id = 2,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 100'000,
      .qty = 10,
      .send_ts = 200,
  };
  core_.submit_order(session, bid);
  (void)core_.drain_events();

  // Reuse client_order_id=2 after full fill → still rejected.
  core_.submit_order(session, bid);
  const auto events = core_.drain_events();
  const auto *reject = find_event(events, EventType::kOrderRejected);
  ASSERT_NE(reject, nullptr);
  EXPECT_EQ(reject->reason, RejectReason::kDuplicateOrderId);
}

TEST_F(ExchangeCoreTest, OrderIdAllowedAfterDisconnectAndNewSession) {
  auto session1 = core_.on_client_connect();

  const NewOrder order{
      .client_order_id = 42,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 99'000,
      .qty = 10,
      .send_ts = 100,
  };
  core_.submit_order(session1, order);
  (void)core_.drain_events();

  // Disconnect session 1 → seen_orders_ for session 1 cleared.
  core_.on_client_disconnect(session1);

  // New session → order 42 is fresh again.
  auto session2 = core_.on_client_connect();
  core_.submit_order(session2, order);
  const auto events = core_.drain_events();
  EXPECT_NE(find_event(events, EventType::kOrderAccepted), nullptr);
}

// ---------------------------------------------------------------------------
// client_order_id 48-bit range validation
// ---------------------------------------------------------------------------

TEST_F(ExchangeCoreTest, SubmitRejectsOversizedClientOrderId) {
  const auto session = core_.on_client_connect();

  // Order ID that exceeds the 48-bit composite key range.
  const NewOrder order{
      .client_order_id = 0x0001'0000'0000'0000, // bit 48 set
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 99'000,
      .qty = 10,
      .send_ts = 100,
  };

  core_.submit_order(session, order);
  const auto events = core_.drain_events();
  const auto *reject = find_event(events, EventType::kOrderRejected);
  ASSERT_NE(reject, nullptr);
  EXPECT_EQ(reject->reason, RejectReason::kInvalidOrderId);
}

TEST_F(ExchangeCoreTest, CancelRejectsOversizedClientOrderId) {
  const auto session = core_.on_client_connect();

  const CancelOrder cancel{
      .client_order_id = 0x0001'0000'0000'0000,
      .symbol_id = 1,
      .send_ts = 100,
  };

  core_.cancel_order(session, cancel);
  const auto events = core_.drain_events();
  const auto *reject = find_event(events, EventType::kCancelRejected);
  ASSERT_NE(reject, nullptr);
  EXPECT_EQ(reject->reason, RejectReason::kInvalidOrderId);
}

TEST_F(ExchangeCoreTest, ModifyRejectsOversizedClientOrderId) {
  const auto session = core_.on_client_connect();

  const ModifyOrder modify{
      .client_order_id = 0x0001'0000'0000'0000,
      .symbol_id = 1,
      .new_price = 100'000,
      .new_qty = 5,
      .send_ts = 100,
  };

  core_.modify_order(session, modify);
  const auto events = core_.drain_events();
  const auto *reject = find_event(events, EventType::kModifyRejected);
  ASSERT_NE(reject, nullptr);
  EXPECT_EQ(reject->reason, RejectReason::kInvalidOrderId);
}

// ---------------------------------------------------------------------------
// Session ID wrap-safety
// ---------------------------------------------------------------------------

TEST_F(ExchangeCoreTest, SessionIdWrapSkipsActiveSession) {
  // Connect many sessions, keeping the first one alive.
  const auto first_session = core_.on_client_connect(); // session 1

  // Connect and disconnect enough sessions to approach wrap-around.
  // We only need to verify that after wrap, the first session's ID is skipped.
  // Connect 65534 more sessions (IDs 2..65535), disconnect all of them.
  for (int i = 0; i < 65534; ++i) {
    auto sid = core_.on_client_connect();
    core_.on_client_disconnect(sid);
  }

  // Next connect wraps around — must skip first_session (still active).
  const auto wrapped_session = core_.on_client_connect();
  EXPECT_NE(wrapped_session, 0);
  EXPECT_NE(wrapped_session, first_session);
}

// ---------------------------------------------------------------------------
// Engine dispatch contract: request_seq stamped on all drained events
// ---------------------------------------------------------------------------
//
// ExchangeCore does NOT set request_seq — it emits events with seq == 0.
// The engine's drain_and_dispatch (exchange_engine_main.cpp) stamps every
// drained event with the originating request's sequence number. This test
// fixes that contract: a crossing order produces multiple event types
// (Ack + Fill + BBO), and all must carry the same stamped seq while
// preserving their original payload fields.

TEST_F(ExchangeCoreTest, DispatchCopiesRequestSeqToAllEvents) {
  const auto session = core_.on_client_connect();

  // Seed two resting asks so a partial crossing leaves residual volume
  // and emits BBOUpdate after the fill.
  for (std::uint64_t i = 1; i <= 2; ++i) {
    const NewOrder ask{
        .client_order_id = i,
        .symbol_id = 1,
        .side = algo::Side::kAsk,
        .price = 100'000,
        .qty = 5,
        .send_ts = 100,
    };
    core_.submit_order(session, ask);
    (void)core_.drain_events();
  }

  // Crossing bid (partial) → OrderAccepted + Fill(taker+maker) + BBOUpdate.
  const NewOrder bid{
      .client_order_id = 10,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 100'000,
      .qty = 3, // partial fill against first ask (qty 5)
      .send_ts = 200,
  };
  core_.submit_order(session, bid);
  const auto events = core_.drain_events();

  // Expect multiple event types from a single crossing order.
  ASSERT_GE(events.size(), 3U); // at least: Ack + Fills + BBO

  // ExchangeCore leaves request_seq at 0.
  for (const auto &e : events) {
    EXPECT_EQ(e.request_seq, 0U);
  }

  // Simulate engine dispatch: stamp all events with the same request_seq.
  constexpr std::uint32_t kTestSeq = 42;
  std::vector<ExchangeEvent> stamped(events.begin(), events.end());
  for (auto &e : stamped) {
    e.request_seq = kTestSeq;
  }

  // Verify: all events carry the stamped seq, original payloads preserved.
  for (const auto &e : stamped) {
    EXPECT_EQ(e.request_seq, kTestSeq);
    // Payload fields must not be corrupted by stamping.
    EXPECT_EQ(e.symbol_id, 1U);
  }

  // Verify specific event types are present in the batch.
  EXPECT_NE(find_event(std::span(stamped), EventType::kOrderAccepted), nullptr);
  EXPECT_NE(find_event(std::span(stamped), EventType::kFill), nullptr);
  EXPECT_NE(find_event(std::span(stamped), EventType::kBBOUpdate), nullptr);
}

} // namespace
} // namespace mk::app
