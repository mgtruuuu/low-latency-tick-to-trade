/**
 * @file order_response_handler_test.cpp
 * @brief Tests for OrderResponseHandler — TCP response dispatch, consecutive
 *        reject kill switch trigger, parse error counting, heartbeat tracking.
 */

#include "tick_to_trade/order_response_handler.hpp"

#include "tick_to_trade/latency_tracker.hpp"
#include "tick_to_trade/order_manager.hpp"
#include "tick_to_trade/strategy_ctx.hpp"
#include "tick_to_trade/tcp_connection.hpp"

#include "tick_to_trade/spread_strategy.hpp"

#include "shared/protocol.hpp"
#include "shared/protocol_codec.hpp"

#include "net/message_codec.hpp"

#include "tick_to_trade/pipeline_log_entry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <span>

namespace mk::app {
namespace {

// ---------------------------------------------------------------------------
// Test constants
// ---------------------------------------------------------------------------

constexpr std::uint32_t kMaxSymbols = 2;
constexpr std::uint32_t kMaxOutstanding = 4;
using TestStrategy = SpreadStrategy<kMaxSymbols>;

constexpr std::int64_t kTimeoutNs = 100'000'000;
constexpr std::size_t kWheelSize = 256;
constexpr std::size_t kMaxTimers = 4;

// ---------------------------------------------------------------------------
// Helpers — build ParsedMessageView from protocol structs
// ---------------------------------------------------------------------------

struct PayloadBuf {
  std::array<std::byte, 128> bytes{};
  std::size_t len = 0;
};

template <typename T, typename SerializeFn>
PayloadBuf serialize_into(const T &msg, SerializeFn fn) {
  PayloadBuf buf;
  buf.len = fn(buf.bytes, msg);
  return buf;
}

net::ParsedMessageView make_view(MsgType type,
                                 std::span<const std::byte> payload) {
  net::ParsedMessageView view;
  view.header.msg_type = static_cast<std::uint16_t>(type);
  view.header.payload_len = static_cast<std::uint32_t>(payload.size());
  view.payload = payload;
  return view;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class OrderResponseHandlerTest : public ::testing::Test {
protected:
  void SetUp() override {
    ctx_ = make_strategy_ctx<TestStrategy>(ctx_buf_.data(), 0, 0, 0,
                                           kMaxOutstanding, kMaxOutstanding,
                                           kWheelSize, kMaxTimers);
    om_ = std::construct_at(reinterpret_cast<OrderManager *>(om_storage_), ctx_,
                            /*max_position=*/1000,
                            /*max_order_size=*/100,
                            /*max_notional=*/10'000'000,
                            /*max_orders_per_window=*/100,
                            /*rate_window_ns=*/1'000'000'000,
                            /*order_timeout_ns=*/kTimeoutNs);
  }

  void TearDown() override { std::destroy_at(om_); }

  // Send a new order through OrderManager and return its client_order_id.
  std::uint64_t send_order() {
    const Signal sig{
        .side = algo::Side::kBid, .price = 10000, .qty = 10, .symbol_id = 1};
    NewOrder order{};
    EXPECT_TRUE(om_->on_signal(sig, order));
    return order.client_order_id;
  }

  // Dispatch a message to the handler.
  bool dispatch(MsgType type, std::span<const std::byte> payload) {
    auto view = make_view(type, payload);
    return handler_.on_tcp_message(
        view, *om_, tracker_, conn_,
        *log_queue_); // NOLINT(bugprone-unchecked-optional-access)
  }

  // -- Members --
  std::array<std::byte, strategy_ctx_buf_size<TestStrategy>(
                            0, 0, 0, kMaxOutstanding, kWheelSize, kMaxTimers)>
      ctx_buf_{};
  StrategyCtx ctx_{};
  alignas(OrderManager) std::byte om_storage_[sizeof(OrderManager)]{};
  OrderManager *om_{nullptr};

  OrderResponseHandler handler_;
  LatencyTracker tracker_;
  ConnectionState conn_;

  // LogQueue: non-owning SPSCQueue backed by stack buffer.
  std::array<LogEntry, 16> log_buf_{};
  std::optional<PipelineLogQueue> log_queue_{
      PipelineLogQueue::create(log_buf_.data(), sizeof(log_buf_), 16)};
};

// ---------------------------------------------------------------------------
// OrderAck dispatch
// ---------------------------------------------------------------------------

TEST_F(OrderResponseHandlerTest, OrderAckDispatchesAndResetsRejectCounter) {
  const auto id = send_order();

  OrderAck ack{};
  ack.client_order_id = id;
  ack.exchange_order_id = 1000;
  ack.send_ts = 0;
  auto payload = serialize_into(ack, serialize_order_ack);
  ASSERT_GT(payload.len, 0U);

  EXPECT_FALSE(
      dispatch(MsgType::kOrderAck, {payload.bytes.data(), payload.len}));
  EXPECT_EQ(handler_.consecutive_rejects(), 0U);
  // OrderAck dispatched — outstanding order should have exchange_id set.
  EXPECT_EQ(om_->outstanding_count(), 1U);
}

// ---------------------------------------------------------------------------
// OrderReject dispatch
// ---------------------------------------------------------------------------

TEST_F(OrderResponseHandlerTest, OrderRejectIncrementsConsecutiveRejects) {
  const auto id = send_order();

  OrderReject rej{};
  rej.client_order_id = id;
  rej.reason = RejectReason::kUnknown;
  rej.send_ts = 0;
  auto payload = serialize_into(rej, serialize_order_reject);
  ASSERT_GT(payload.len, 0U);

  EXPECT_FALSE(
      dispatch(MsgType::kOrderReject, {payload.bytes.data(), payload.len}));
  EXPECT_EQ(handler_.consecutive_rejects(), 1U);
}

TEST_F(OrderResponseHandlerTest, FiveConsecutiveRejectsTriggersKillSwitch) {
  // Send 5 orders and reject all of them.
  // Need 5 orders but max_outstanding=4, so reject first 4, then send 5th.
  for (int i = 0; i < 4; ++i) {
    const auto id = send_order();
    OrderReject rej{};
    rej.client_order_id = id;
    rej.reason = RejectReason::kUnknown;
    rej.send_ts = 0;
    auto payload = serialize_into(rej, serialize_order_reject);

    const bool kill =
        dispatch(MsgType::kOrderReject, {payload.bytes.data(), payload.len});
    EXPECT_FALSE(kill) << "Should not trigger at reject #" << (i + 1);
  }
  EXPECT_EQ(handler_.consecutive_rejects(), 4U);
  EXPECT_EQ(om_->outstanding_count(), 0U);

  // 5th reject triggers kill switch.
  const auto id5 = send_order();
  OrderReject rej5{};
  rej5.client_order_id = id5;
  rej5.reason = RejectReason::kUnknown;
  rej5.send_ts = 0;
  auto payload5 = serialize_into(rej5, serialize_order_reject);

  EXPECT_TRUE(
      dispatch(MsgType::kOrderReject, {payload5.bytes.data(), payload5.len}));
  EXPECT_EQ(handler_.consecutive_rejects(), 5U);
}

TEST_F(OrderResponseHandlerTest, AckResetsRejectCounterBeforeThreshold) {
  // 3 rejects, then 1 ack, then 3 more rejects — should NOT trigger kill.
  for (int i = 0; i < 3; ++i) {
    const auto id = send_order();
    OrderReject rej{};
    rej.client_order_id = id;
    rej.reason = RejectReason::kUnknown;
    rej.send_ts = 0;
    auto payload = serialize_into(rej, serialize_order_reject);
    EXPECT_FALSE(
        dispatch(MsgType::kOrderReject, {payload.bytes.data(), payload.len}));
  }
  EXPECT_EQ(handler_.consecutive_rejects(), 3U);

  // Ack resets counter.
  const auto ack_id = send_order();
  OrderAck ack{};
  ack.client_order_id = ack_id;
  ack.exchange_order_id = 2000;
  ack.send_ts = 0;
  auto ack_payload = serialize_into(ack, serialize_order_ack);
  EXPECT_FALSE(dispatch(MsgType::kOrderAck,
                        {ack_payload.bytes.data(), ack_payload.len}));
  EXPECT_EQ(handler_.consecutive_rejects(), 0U);
}

// ---------------------------------------------------------------------------
// FillReport dispatch
// ---------------------------------------------------------------------------

TEST_F(OrderResponseHandlerTest, FillReportDispatchesAndResetsRejects) {
  const auto id = send_order();

  FillReport fill{};
  fill.client_order_id = id;
  fill.exchange_order_id = 1000;
  fill.fill_price = 10000;
  fill.fill_qty = 10;
  fill.remaining_qty = 0;
  fill.send_ts = 0;
  auto payload = serialize_into(fill, serialize_fill_report);
  ASSERT_GT(payload.len, 0U);

  EXPECT_FALSE(
      dispatch(MsgType::kFillReport, {payload.bytes.data(), payload.len}));
  EXPECT_EQ(handler_.consecutive_rejects(), 0U);
  EXPECT_EQ(om_->outstanding_count(), 0U);
}

TEST_F(OrderResponseHandlerTest, FillResetsConsecutiveRejectCounter) {
  // 2 rejects, then fill — counter resets.
  const auto id1 = send_order();
  const auto id2 = send_order();

  for (auto id : {id1, id2}) {
    OrderReject rej{};
    rej.client_order_id = id;
    rej.reason = RejectReason::kUnknown;
    rej.send_ts = 0;
    auto payload = serialize_into(rej, serialize_order_reject);
    dispatch(MsgType::kOrderReject, {payload.bytes.data(), payload.len});
  }
  EXPECT_EQ(handler_.consecutive_rejects(), 2U);

  const auto id3 = send_order();
  FillReport fill{};
  fill.client_order_id = id3;
  fill.exchange_order_id = 1000;
  fill.fill_price = 10000;
  fill.fill_qty = 10;
  fill.remaining_qty = 0;
  fill.send_ts = 0;
  auto payload = serialize_into(fill, serialize_fill_report);
  dispatch(MsgType::kFillReport, {payload.bytes.data(), payload.len});
  EXPECT_EQ(handler_.consecutive_rejects(), 0U);
}

// ---------------------------------------------------------------------------
// CancelAck / CancelReject dispatch
// ---------------------------------------------------------------------------

TEST_F(OrderResponseHandlerTest, CancelAckDispatches) {
  const auto id = send_order();
  // Need to ack first to allow cancel.
  OrderAck ack{};
  ack.client_order_id = id;
  ack.exchange_order_id = 1000;
  ack.send_ts = 0;
  auto ack_payload = serialize_into(ack, serialize_order_ack);
  dispatch(MsgType::kOrderAck, {ack_payload.bytes.data(), ack_payload.len});

  CancelAck cancel_ack{};
  cancel_ack.client_order_id = id;
  cancel_ack.send_ts = 0;
  auto payload = serialize_into(cancel_ack, serialize_cancel_ack);
  ASSERT_GT(payload.len, 0U);

  EXPECT_FALSE(
      dispatch(MsgType::kCancelAck, {payload.bytes.data(), payload.len}));
  EXPECT_EQ(om_->outstanding_count(), 0U);
}

TEST_F(OrderResponseHandlerTest, CancelRejectDispatches) {
  const auto id = send_order();

  CancelReject cr{};
  cr.client_order_id = id;
  cr.reason = RejectReason::kUnknown;
  cr.send_ts = 0;
  auto payload = serialize_into(cr, serialize_cancel_reject);
  ASSERT_GT(payload.len, 0U);

  EXPECT_FALSE(
      dispatch(MsgType::kCancelReject, {payload.bytes.data(), payload.len}));
}

// ---------------------------------------------------------------------------
// ModifyAck / ModifyReject dispatch
// ---------------------------------------------------------------------------

TEST_F(OrderResponseHandlerTest, ModifyAckDispatches) {
  const auto id = send_order();

  ModifyAck mack{};
  mack.client_order_id = id;
  mack.new_exchange_order_id = 2000;
  mack.send_ts = 0;
  auto payload = serialize_into(mack, serialize_modify_ack);
  ASSERT_GT(payload.len, 0U);

  EXPECT_FALSE(
      dispatch(MsgType::kModifyAck, {payload.bytes.data(), payload.len}));
  EXPECT_EQ(om_->modifies_acked(), 1U);
}

TEST_F(OrderResponseHandlerTest, ModifyRejectDispatches) {
  const auto id = send_order();

  ModifyReject mrej{};
  mrej.client_order_id = id;
  mrej.reason = RejectReason::kUnknown;
  mrej.send_ts = 0;
  auto payload = serialize_into(mrej, serialize_modify_reject);
  ASSERT_GT(payload.len, 0U);

  EXPECT_FALSE(
      dispatch(MsgType::kModifyReject, {payload.bytes.data(), payload.len}));
}

// ---------------------------------------------------------------------------
// HeartbeatAck
// ---------------------------------------------------------------------------

TEST_F(OrderResponseHandlerTest, HeartbeatAckUpdatesConnectionState) {
  const auto before_recv = conn_.heartbeats_recv.load();
  const auto before_ts = conn_.last_hb_recv;

  EXPECT_FALSE(dispatch(MsgType::kHeartbeatAck, {}));

  EXPECT_EQ(conn_.heartbeats_recv.load(), before_recv + 1);
  EXPECT_GT(conn_.last_hb_recv, before_ts);
}

// ---------------------------------------------------------------------------
// Unknown message type
// ---------------------------------------------------------------------------

TEST_F(OrderResponseHandlerTest, UnknownMsgTypeIncrementsCounter) {
  // Use a fake msg_type value (255).
  net::ParsedMessageView view;
  view.header.msg_type = 255;
  view.header.payload_len = 0;
  view.payload = {};

  EXPECT_FALSE(handler_.on_tcp_message(
      view, *om_, tracker_, conn_,
      *log_queue_)); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(handler_.unknown_types(), 1U);
}

// ---------------------------------------------------------------------------
// Parse errors — short payload
// ---------------------------------------------------------------------------

TEST_F(OrderResponseHandlerTest, ShortOrderAckPayloadIncrementsParseErrors) {
  // 2-byte payload is too short for OrderAck.
  std::array<std::byte, 2> garbage{};
  EXPECT_FALSE(dispatch(MsgType::kOrderAck, garbage));
  EXPECT_EQ(handler_.parse_errors(), 1U);
}

TEST_F(OrderResponseHandlerTest, ShortFillPayloadIncrementsParseErrors) {
  std::array<std::byte, 2> garbage{};
  EXPECT_FALSE(dispatch(MsgType::kFillReport, garbage));
  EXPECT_EQ(handler_.parse_errors(), 1U);
}

TEST_F(OrderResponseHandlerTest, ShortRejectPayloadIncrementsParseErrors) {
  std::array<std::byte, 2> garbage{};
  EXPECT_FALSE(dispatch(MsgType::kOrderReject, garbage));
  EXPECT_EQ(handler_.parse_errors(), 1U);
  // Consecutive rejects should NOT increment on parse failure.
  EXPECT_EQ(handler_.consecutive_rejects(), 0U);
}

TEST_F(OrderResponseHandlerTest, ShortCancelAckPayloadIncrementsParseErrors) {
  std::array<std::byte, 2> garbage{};
  EXPECT_FALSE(dispatch(MsgType::kCancelAck, garbage));
  EXPECT_EQ(handler_.parse_errors(), 1U);
}

TEST_F(OrderResponseHandlerTest, ShortModifyAckPayloadIncrementsParseErrors) {
  std::array<std::byte, 2> garbage{};
  EXPECT_FALSE(dispatch(MsgType::kModifyAck, garbage));
  EXPECT_EQ(handler_.parse_errors(), 1U);
}

} // namespace
} // namespace mk::app
