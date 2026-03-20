/**
 * @file replay_order_gateway_test.cpp
 * @brief Tests for ReplayOrderGateway — immediate ack+fill, cancel-always-reject,
 *        heartbeat roundtrip, buffer guards, and malformed input handling.
 */

#include "simulated_exchange/replay_order_gateway.hpp"

#include "shared/protocol.hpp"
#include "shared/protocol_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <span>

namespace mk::app {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct PackedFrame {
  std::array<std::byte, 128> bytes{};
  std::size_t len = 0;
};

PackedFrame pack_new_order(const NewOrder &order) {
  PackedFrame out;
  std::array<std::byte, kNewOrderWireSize> payload{};
  const auto payload_len = serialize_new_order(payload, order);
  if (payload_len == 0) {
    return out;
  }
  out.len = pack_tcp_message(
      out.bytes, MsgType::kNewOrder,
      std::span<const std::byte>(payload.data(), payload_len));
  return out;
}

PackedFrame pack_cancel_order(const CancelOrder &cancel) {
  PackedFrame out;
  std::array<std::byte, kCancelOrderWireSize> payload{};
  const auto payload_len = serialize_cancel_order(payload, cancel);
  if (payload_len == 0) {
    return out;
  }
  out.len = pack_tcp_message(
      out.bytes, MsgType::kCancelOrder,
      std::span<const std::byte>(payload.data(), payload_len));
  return out;
}

PackedFrame pack_heartbeat() {
  PackedFrame out;
  out.len = pack_tcp_message(out.bytes, MsgType::kHeartbeat, {});
  return out;
}

bool parse_tlv(std::span<const std::byte> raw, net::ParsedMessageView &out) {
  return net::unpack_message(raw, out);
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class ReplayOrderGatewayTest : public ::testing::Test {
protected:
  static constexpr std::size_t kTxCap = ReplayOrderGateway::kMaxResponseBytes;
  ReplayOrderGateway gw_;
};

// ---------------------------------------------------------------------------
// NewOrder — ack + full fill roundtrip
// ---------------------------------------------------------------------------

TEST_F(ReplayOrderGatewayTest, NewOrderReturnsAckAndFill) {
  const NewOrder req{
      .client_order_id = 42,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 100'000,
      .qty = 10,
      .send_ts = 1111,
  };
  const auto frame = pack_new_order(req);
  ASSERT_GT(frame.len, 0U);

  std::array<std::byte, kTxCap> tx{};
  const auto written = gw_.on_message(
      std::span<const std::byte>(frame.bytes.data(), frame.len), tx);
  ASSERT_GT(written, 0U);

  // First TLV: OrderAck
  net::ParsedMessageView ack_msg;
  ASSERT_TRUE(parse_tlv(std::span<const std::byte>(tx.data(), written),
                         ack_msg));
  ASSERT_EQ(static_cast<MsgType>(ack_msg.header.msg_type), MsgType::kOrderAck);

  OrderAck ack;
  ASSERT_TRUE(deserialize_order_ack(ack_msg.payload, ack));
  EXPECT_EQ(ack.client_order_id, req.client_order_id);
  EXPECT_EQ(ack.exchange_order_id, 1U); // first exchange id
  EXPECT_EQ(ack.send_ts, req.send_ts);

  // Second TLV: FillReport
  const auto ack_consumed =
      net::kMessageHeaderSize + ack_msg.header.payload_len;
  ASSERT_GT(written, ack_consumed);

  net::ParsedMessageView fill_msg;
  ASSERT_TRUE(parse_tlv(
      std::span<const std::byte>(tx.data() + ack_consumed,
                                  written - ack_consumed),
      fill_msg));
  ASSERT_EQ(static_cast<MsgType>(fill_msg.header.msg_type),
            MsgType::kFillReport);

  FillReport fill;
  ASSERT_TRUE(deserialize_fill_report(fill_msg.payload, fill));
  EXPECT_EQ(fill.client_order_id, req.client_order_id);
  EXPECT_EQ(fill.exchange_order_id, 1U);
  EXPECT_EQ(fill.fill_price, req.price);
  EXPECT_EQ(fill.fill_qty, req.qty);
  EXPECT_EQ(fill.remaining_qty, 0U); // fully filled
  EXPECT_EQ(fill.send_ts, req.send_ts);
}

TEST_F(ReplayOrderGatewayTest, ExchangeIdMonotonicallyIncreases) {
  const NewOrder req{
      .client_order_id = 1,
      .symbol_id = 1,
      .side = algo::Side::kAsk,
      .price = 200'000,
      .qty = 5,
      .send_ts = 0,
  };
  const auto frame = pack_new_order(req);
  ASSERT_GT(frame.len, 0U);

  std::uint64_t prev_id = 0;
  for (int i = 0; i < 5; ++i) {
    std::array<std::byte, kTxCap> tx{};
    const auto written = gw_.on_message(
        std::span<const std::byte>(frame.bytes.data(), frame.len), tx);
    ASSERT_GT(written, 0U);

    net::ParsedMessageView msg;
    ASSERT_TRUE(parse_tlv(std::span<const std::byte>(tx.data(), written), msg));
    OrderAck ack;
    ASSERT_TRUE(deserialize_order_ack(msg.payload, ack));

    EXPECT_GT(ack.exchange_order_id, prev_id);
    prev_id = ack.exchange_order_id;
  }
}

// ---------------------------------------------------------------------------
// CancelOrder — always rejected in replay mode
// ---------------------------------------------------------------------------

TEST_F(ReplayOrderGatewayTest, CancelAlwaysRejected) {
  const CancelOrder cancel{
      .client_order_id = 99,
      .symbol_id = 1,
      .send_ts = 2222,
  };
  const auto frame = pack_cancel_order(cancel);
  ASSERT_GT(frame.len, 0U);

  std::array<std::byte, 256> tx{};
  const auto written = gw_.on_message(
      std::span<const std::byte>(frame.bytes.data(), frame.len), tx);
  ASSERT_GT(written, 0U);

  net::ParsedMessageView msg;
  ASSERT_TRUE(parse_tlv(std::span<const std::byte>(tx.data(), written), msg));
  ASSERT_EQ(static_cast<MsgType>(msg.header.msg_type),
            MsgType::kCancelReject);

  CancelReject cr;
  ASSERT_TRUE(deserialize_cancel_reject(msg.payload, cr));
  EXPECT_EQ(cr.client_order_id, cancel.client_order_id);
  EXPECT_EQ(cr.send_ts, cancel.send_ts);
}

// ---------------------------------------------------------------------------
// Heartbeat — returns HeartbeatAck
// ---------------------------------------------------------------------------

TEST_F(ReplayOrderGatewayTest, HeartbeatReturnsAck) {
  const auto frame = pack_heartbeat();
  ASSERT_GT(frame.len, 0U);

  std::array<std::byte, 256> tx{};
  const auto written = gw_.on_message(
      std::span<const std::byte>(frame.bytes.data(), frame.len), tx);
  ASSERT_GT(written, 0U);

  net::ParsedMessageView msg;
  ASSERT_TRUE(parse_tlv(std::span<const std::byte>(tx.data(), written), msg));
  EXPECT_EQ(static_cast<MsgType>(msg.header.msg_type),
            MsgType::kHeartbeatAck);
  EXPECT_EQ(msg.header.payload_len, 0U);
}

// ---------------------------------------------------------------------------
// Unknown message type — silently ignored
// ---------------------------------------------------------------------------

TEST_F(ReplayOrderGatewayTest, UnknownMsgTypeReturnsZero) {
  // Craft a valid TLV frame with an unknown msg_type (255).
  std::array<std::byte, net::kMessageHeaderSize> raw{};
  const auto packed = net::pack_message(
      raw, kProtocolVersion, static_cast<std::uint16_t>(255), 0, {});
  ASSERT_GT(packed, 0U);

  std::array<std::byte, 256> tx{};
  const auto written = gw_.on_message(
      std::span<const std::byte>(raw.data(), packed), tx);
  EXPECT_EQ(written, 0U);
}

// ---------------------------------------------------------------------------
// Malformed input — TLV parse failure
// ---------------------------------------------------------------------------

TEST_F(ReplayOrderGatewayTest, MalformedTlvReturnsZero) {
  // Too short to be a valid TLV frame.
  std::array<std::byte, 4> garbage{};
  std::array<std::byte, 256> tx{};
  const auto written = gw_.on_message(
      std::span<const std::byte>(garbage.data(), garbage.size()), tx);
  EXPECT_EQ(written, 0U);
}

// ---------------------------------------------------------------------------
// Buffer too small — returns zero without processing
// ---------------------------------------------------------------------------

TEST_F(ReplayOrderGatewayTest, TooSmallBufferForNewOrderReturnsZero) {
  const NewOrder req{
      .client_order_id = 1,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 50'000,
      .qty = 1,
      .send_ts = 0,
  };
  const auto frame = pack_new_order(req);
  ASSERT_GT(frame.len, 0U);

  // Buffer too small for ack + fill response.
  std::array<std::byte, 32> tiny{};
  const auto written = gw_.on_message(
      std::span<const std::byte>(frame.bytes.data(), frame.len), tiny);
  EXPECT_EQ(written, 0U);
}

TEST_F(ReplayOrderGatewayTest, TooSmallBufferForCancelReturnsZero) {
  const CancelOrder cancel{
      .client_order_id = 1,
      .symbol_id = 1,
      .send_ts = 0,
  };
  const auto frame = pack_cancel_order(cancel);
  ASSERT_GT(frame.len, 0U);

  // Buffer smaller than CancelReject TLV.
  std::array<std::byte, 8> tiny{};
  const auto written = gw_.on_message(
      std::span<const std::byte>(frame.bytes.data(), frame.len), tiny);
  EXPECT_EQ(written, 0U);
}

} // namespace
} // namespace mk::app
