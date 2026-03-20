/**
 * @file order_gateway_test.cpp
 * @brief Tests for OrderGateway — session isolation, modify, heartbeat,
 *        crossing fills, invalid symbol, buffer guards.
 */

#include "simulated_exchange/order_gateway.hpp"

#include "shared/protocol.hpp"
#include "shared/protocol_codec.hpp"

#include <arpa/inet.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <span>
#include <vector>

namespace mk::app {
namespace {

struct MockUdpSocket {
  enum class SendtoStatus { kOk, kWouldBlock, kError };

  struct SendtoResult {
    SendtoStatus status = SendtoStatus::kError;
    int err_no = 0;
  };

  struct Packet {
    std::vector<std::byte> data;
    sockaddr_in dest;
  };

  std::vector<Packet> sent_packets;

  SendtoResult sendto_nonblocking(const char *buf, std::size_t len,
                                  const sockaddr_in &dest) noexcept {
    sent_packets.push_back(
        {.data = {reinterpret_cast<const std::byte *>(buf),
                  reinterpret_cast<const std::byte *>(buf) + len},
         .dest = dest});
    return {.status = SendtoStatus::kOk, .err_no = 0};
  }
};

static_assert(net::UdpSendable<MockUdpSocket>);

sockaddr_in make_addr(const char *ip, std::uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, ip, &addr.sin_addr);
  return addr;
}

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

PackedFrame pack_modify_order(const ModifyOrder &modify) {
  PackedFrame out;
  std::array<std::byte, kModifyOrderWireSize> payload{};
  const auto payload_len = serialize_modify_order(payload, modify);
  if (payload_len == 0) {
    return out;
  }
  out.len = pack_tcp_message(
      out.bytes, MsgType::kModifyOrder,
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

class OrderGatewayTest : public ::testing::Test {
protected:
  static constexpr std::size_t kGatewayTxCap =
      OrderGateway<MockUdpSocket>::kMaxResponseBytes;
  static constexpr std::size_t kExpectedGatewayTxCap =
      (net::kMessageHeaderSize + kOrderAckWireSize) +
      (algo::MatchingEngine<>::kMaxFillsPerMatch *
       (net::kMessageHeaderSize + kFillReportWireSize));
  static_assert(kGatewayTxCap == kExpectedGatewayTxCap);

  OrderGatewayTest() : publisher_(sock_, /*symbol_id=*/1, seq_, feed_a_) {
    gateway_.register_symbol(1, publisher_);
  }

  MockUdpSocket sock_;
  std::uint64_t seq_ = 1;
  sockaddr_in feed_a_ = make_addr("239.255.0.1", 9000);
  MarketDataPublisher<MockUdpSocket> publisher_;
  OrderGateway<MockUdpSocket> gateway_;
};

TEST_F(OrderGatewayTest, SameClientOrderIdAcrossSessionsIsAccepted) {
  const auto session1 = gateway_.on_client_connect();
  const auto session2 = gateway_.on_client_connect();
  ASSERT_NE(session1, 0U);
  ASSERT_NE(session2, 0U);
  ASSERT_NE(session1, session2);

  const NewOrder req1{
      .client_order_id = 42,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 100'000,
      .qty = 10,
      .send_ts = 111,
  };
  const NewOrder req2{
      .client_order_id = 42, // same ClOrdID, different session
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 100'100,
      .qty = 12,
      .send_ts = 222,
  };

  const auto frame1 = pack_new_order(req1);
  const auto frame2 = pack_new_order(req2);
  ASSERT_GT(frame1.len, 0U);
  ASSERT_GT(frame2.len, 0U);

  std::array<std::byte, kGatewayTxCap> tx1{};
  const auto written1 = gateway_.on_message(
      session1, std::span<const std::byte>(frame1.bytes.data(), frame1.len), tx1);
  ASSERT_GT(written1, 0U);

  net::ParsedMessageView parsed1;
  ASSERT_TRUE(parse_tlv(std::span<const std::byte>(tx1.data(), written1), parsed1));
  ASSERT_EQ(static_cast<MsgType>(parsed1.header.msg_type), MsgType::kOrderAck);
  OrderAck ack1;
  ASSERT_TRUE(deserialize_order_ack(parsed1.payload, ack1));
  EXPECT_EQ(ack1.client_order_id, req1.client_order_id);

  std::array<std::byte, kGatewayTxCap> tx2{};
  const auto written2 = gateway_.on_message(
      session2, std::span<const std::byte>(frame2.bytes.data(), frame2.len), tx2);
  ASSERT_GT(written2, 0U);

  net::ParsedMessageView parsed2;
  ASSERT_TRUE(parse_tlv(std::span<const std::byte>(tx2.data(), written2), parsed2));
  ASSERT_EQ(static_cast<MsgType>(parsed2.header.msg_type), MsgType::kOrderAck);
  OrderAck ack2;
  ASSERT_TRUE(deserialize_order_ack(parsed2.payload, ack2));
  EXPECT_EQ(ack2.client_order_id, req2.client_order_id);
  EXPECT_NE(ack1.exchange_order_id, ack2.exchange_order_id);
}

TEST_F(OrderGatewayTest, DisconnectOneSessionKeepsOtherSessionOrdersAlive) {
  const auto session1 = gateway_.on_client_connect();
  const auto session2 = gateway_.on_client_connect();

  const NewOrder req{
      .client_order_id = 7,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 99'900,
      .qty = 5,
      .send_ts = 1000,
  };
  const auto new_frame = pack_new_order(req);
  ASSERT_GT(new_frame.len, 0U);

  std::array<std::byte, kGatewayTxCap> tx_new1{};
  std::array<std::byte, kGatewayTxCap> tx_new2{};
  EXPECT_GT(gateway_.on_message(
                session1,
                std::span<const std::byte>(new_frame.bytes.data(), new_frame.len),
                tx_new1),
            0U);
  EXPECT_GT(gateway_.on_message(
                session2,
                std::span<const std::byte>(new_frame.bytes.data(), new_frame.len),
                tx_new2),
            0U);

  // Disconnect only session1. Session2's order must remain cancelable.
  gateway_.on_client_disconnect(session1);

  const CancelOrder cancel2{
      .client_order_id = req.client_order_id,
      .symbol_id = 1,
      .send_ts = 2000,
  };
  const auto cancel_frame = pack_cancel_order(cancel2);
  ASSERT_GT(cancel_frame.len, 0U);

  std::array<std::byte, 512> tx_cancel{};
  const auto cancel_written = gateway_.on_message(
      session2,
      std::span<const std::byte>(cancel_frame.bytes.data(), cancel_frame.len),
      tx_cancel);
  ASSERT_GT(cancel_written, 0U);

  net::ParsedMessageView parsed;
  ASSERT_TRUE(parse_tlv(std::span<const std::byte>(tx_cancel.data(), cancel_written),
                        parsed));
  EXPECT_EQ(static_cast<MsgType>(parsed.header.msg_type), MsgType::kCancelAck);
  CancelAck ack;
  ASSERT_TRUE(deserialize_cancel_ack(parsed.payload, ack));
  EXPECT_EQ(ack.client_order_id, cancel2.client_order_id);
}

TEST_F(OrderGatewayTest, TooSmallResponseBufferDoesNotMutateOrderState) {
  const auto session = gateway_.on_client_connect();
  ASSERT_NE(session, 0U);

  const NewOrder req{
      .client_order_id = 99,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 101'000,
      .qty = 3,
      .send_ts = 1234,
  };
  const auto new_frame = pack_new_order(req);
  ASSERT_GT(new_frame.len, 0U);

  // Intentionally too small for NewOrder worst-case response.
  std::array<std::byte, 32> tiny_tx{};
  const auto written = gateway_.on_message(
      session, std::span<const std::byte>(new_frame.bytes.data(), new_frame.len),
      tiny_tx);
  EXPECT_EQ(written, 0U);

  // If state was not mutated, cancel should be rejected as unknown.
  const CancelOrder cancel{
      .client_order_id = req.client_order_id,
      .symbol_id = req.symbol_id,
      .send_ts = 2222,
  };
  const auto cancel_frame = pack_cancel_order(cancel);
  ASSERT_GT(cancel_frame.len, 0U);

  std::array<std::byte, 512> tx_cancel{};
  const auto cancel_written = gateway_.on_message(
      session,
      std::span<const std::byte>(cancel_frame.bytes.data(), cancel_frame.len),
      tx_cancel);
  ASSERT_GT(cancel_written, 0U);

  net::ParsedMessageView parsed;
  ASSERT_TRUE(parse_tlv(std::span<const std::byte>(tx_cancel.data(), cancel_written),
                        parsed));
  ASSERT_EQ(static_cast<MsgType>(parsed.header.msg_type), MsgType::kCancelReject);
  CancelReject rej;
  ASSERT_TRUE(deserialize_cancel_reject(parsed.payload, rej));
  EXPECT_EQ(rej.client_order_id, cancel.client_order_id);
}

// ---------------------------------------------------------------------------
// ModifyOrder — resting order modified at new price
// ---------------------------------------------------------------------------

TEST_F(OrderGatewayTest, ModifyRestingOrderReturnsModifyAck) {
  const auto session = gateway_.on_client_connect();

  // Place a resting bid.
  const NewOrder req{
      .client_order_id = 1,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 90'000,
      .qty = 10,
      .send_ts = 100,
  };
  const auto new_frame = pack_new_order(req);
  ASSERT_GT(new_frame.len, 0U);

  std::array<std::byte, kGatewayTxCap> tx_new{};
  ASSERT_GT(
      gateway_.on_message(
          session,
          std::span<const std::byte>(new_frame.bytes.data(), new_frame.len),
          tx_new),
      0U);

  // Modify to a new price (still below any ask, so it rests).
  const ModifyOrder modify{
      .client_order_id = 1,
      .symbol_id = 1,
      .new_price = 91'000,
      .new_qty = 8,
      .send_ts = 200,
  };
  const auto mod_frame = pack_modify_order(modify);
  ASSERT_GT(mod_frame.len, 0U);

  std::array<std::byte, kGatewayTxCap> tx_mod{};
  const auto written = gateway_.on_message(
      session,
      std::span<const std::byte>(mod_frame.bytes.data(), mod_frame.len),
      tx_mod);
  ASSERT_GT(written, 0U);

  // First TLV should be ModifyAck.
  net::ParsedMessageView parsed;
  ASSERT_TRUE(
      parse_tlv(std::span<const std::byte>(tx_mod.data(), written), parsed));
  ASSERT_EQ(static_cast<MsgType>(parsed.header.msg_type),
            MsgType::kModifyAck);

  ModifyAck ack;
  ASSERT_TRUE(deserialize_modify_ack(parsed.payload, ack));
  EXPECT_EQ(ack.client_order_id, modify.client_order_id);
  EXPECT_GT(ack.new_exchange_order_id, 0U);
  EXPECT_EQ(ack.send_ts, modify.send_ts);
}

// ---------------------------------------------------------------------------
// ModifyOrder — unknown order returns ModifyReject
// ---------------------------------------------------------------------------

TEST_F(OrderGatewayTest, ModifyUnknownOrderReturnsReject) {
  const auto session = gateway_.on_client_connect();

  const ModifyOrder modify{
      .client_order_id = 999, // never placed
      .symbol_id = 1,
      .new_price = 100'000,
      .new_qty = 5,
      .send_ts = 300,
  };
  const auto frame = pack_modify_order(modify);
  ASSERT_GT(frame.len, 0U);

  std::array<std::byte, kGatewayTxCap> tx{};
  const auto written = gateway_.on_message(
      session,
      std::span<const std::byte>(frame.bytes.data(), frame.len), tx);
  ASSERT_GT(written, 0U);

  net::ParsedMessageView parsed;
  ASSERT_TRUE(
      parse_tlv(std::span<const std::byte>(tx.data(), written), parsed));
  ASSERT_EQ(static_cast<MsgType>(parsed.header.msg_type),
            MsgType::kModifyReject);

  ModifyReject rej;
  ASSERT_TRUE(deserialize_modify_reject(parsed.payload, rej));
  EXPECT_EQ(rej.client_order_id, modify.client_order_id);
}

// ---------------------------------------------------------------------------
// ModifyOrder — modified price crosses opposite side, produces fill
// ---------------------------------------------------------------------------

TEST_F(OrderGatewayTest, ModifyThatCrossesProducesFill) {
  const auto session = gateway_.on_client_connect();

  // Place a resting ask at 100'000.
  const NewOrder ask{
      .client_order_id = 1,
      .symbol_id = 1,
      .side = algo::Side::kAsk,
      .price = 100'000,
      .qty = 5,
      .send_ts = 100,
  };
  const auto ask_frame = pack_new_order(ask);
  ASSERT_GT(ask_frame.len, 0U);
  std::array<std::byte, kGatewayTxCap> tx_ask{};
  ASSERT_GT(
      gateway_.on_message(
          session,
          std::span<const std::byte>(ask_frame.bytes.data(), ask_frame.len),
          tx_ask),
      0U);

  // Place a resting bid at 90'000.
  const NewOrder bid{
      .client_order_id = 2,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 90'000,
      .qty = 3,
      .send_ts = 200,
  };
  const auto bid_frame = pack_new_order(bid);
  ASSERT_GT(bid_frame.len, 0U);
  std::array<std::byte, kGatewayTxCap> tx_bid{};
  ASSERT_GT(
      gateway_.on_message(
          session,
          std::span<const std::byte>(bid_frame.bytes.data(), bid_frame.len),
          tx_bid),
      0U);

  // Modify the bid up to 100'000 — crosses the resting ask at 100'000.
  const ModifyOrder modify{
      .client_order_id = 2,
      .symbol_id = 1,
      .new_price = 100'000,
      .new_qty = 3,
      .send_ts = 300,
  };
  const auto mod_frame = pack_modify_order(modify);
  ASSERT_GT(mod_frame.len, 0U);

  std::array<std::byte, kGatewayTxCap> tx_mod{};
  const auto written = gateway_.on_message(
      session,
      std::span<const std::byte>(mod_frame.bytes.data(), mod_frame.len),
      tx_mod);
  ASSERT_GT(written, 0U);

  // First TLV: ModifyAck.
  net::ParsedMessageView ack_msg;
  ASSERT_TRUE(
      parse_tlv(std::span<const std::byte>(tx_mod.data(), written), ack_msg));
  ASSERT_EQ(static_cast<MsgType>(ack_msg.header.msg_type),
            MsgType::kModifyAck);

  // Second TLV: FillReport.
  const auto ack_consumed =
      net::kMessageHeaderSize + ack_msg.header.payload_len;
  ASSERT_GT(written, ack_consumed);

  net::ParsedMessageView fill_msg;
  ASSERT_TRUE(parse_tlv(
      std::span<const std::byte>(tx_mod.data() + ack_consumed,
                                  written - ack_consumed),
      fill_msg));
  ASSERT_EQ(static_cast<MsgType>(fill_msg.header.msg_type),
            MsgType::kFillReport);

  FillReport fill;
  ASSERT_TRUE(deserialize_fill_report(fill_msg.payload, fill));
  EXPECT_EQ(fill.client_order_id, modify.client_order_id);
  EXPECT_EQ(fill.fill_price, 100'000); // resting ask price
  EXPECT_EQ(fill.fill_qty, 3U);        // bid qty fully matched
}

// ---------------------------------------------------------------------------
// Heartbeat — returns HeartbeatAck
// ---------------------------------------------------------------------------

TEST_F(OrderGatewayTest, HeartbeatReturnsAck) {
  const auto session = gateway_.on_client_connect();
  const auto frame = pack_heartbeat();
  ASSERT_GT(frame.len, 0U);

  std::array<std::byte, 256> tx{};
  const auto written = gateway_.on_message(
      session,
      std::span<const std::byte>(frame.bytes.data(), frame.len), tx);
  ASSERT_GT(written, 0U);

  net::ParsedMessageView parsed;
  ASSERT_TRUE(
      parse_tlv(std::span<const std::byte>(tx.data(), written), parsed));
  EXPECT_EQ(static_cast<MsgType>(parsed.header.msg_type),
            MsgType::kHeartbeatAck);
  EXPECT_EQ(parsed.header.payload_len, 0U);
}

// ---------------------------------------------------------------------------
// NewOrder that crosses — immediate fill + resting remainder
// ---------------------------------------------------------------------------

TEST_F(OrderGatewayTest, CrossingOrderProducesAckAndFills) {
  const auto session = gateway_.on_client_connect();

  // Resting ask at 100'000, qty=5.
  const NewOrder ask{
      .client_order_id = 1,
      .symbol_id = 1,
      .side = algo::Side::kAsk,
      .price = 100'000,
      .qty = 5,
      .send_ts = 100,
  };
  const auto ask_frame = pack_new_order(ask);
  std::array<std::byte, kGatewayTxCap> tx_ask{};
  ASSERT_GT(
      gateway_.on_message(
          session,
          std::span<const std::byte>(ask_frame.bytes.data(), ask_frame.len),
          tx_ask),
      0U);

  // Aggressive bid at 100'000, qty=3 — should fully fill against ask.
  const NewOrder bid{
      .client_order_id = 2,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 100'000,
      .qty = 3,
      .send_ts = 200,
  };
  const auto bid_frame = pack_new_order(bid);
  std::array<std::byte, kGatewayTxCap> tx_bid{};
  const auto written = gateway_.on_message(
      session,
      std::span<const std::byte>(bid_frame.bytes.data(), bid_frame.len),
      tx_bid);
  ASSERT_GT(written, 0U);

  // First TLV: OrderAck for the bid.
  net::ParsedMessageView ack_msg;
  ASSERT_TRUE(
      parse_tlv(std::span<const std::byte>(tx_bid.data(), written), ack_msg));
  ASSERT_EQ(static_cast<MsgType>(ack_msg.header.msg_type),
            MsgType::kOrderAck);

  // Second TLV: FillReport.
  const auto ack_consumed =
      net::kMessageHeaderSize + ack_msg.header.payload_len;
  ASSERT_GT(written, ack_consumed);

  net::ParsedMessageView fill_msg;
  ASSERT_TRUE(parse_tlv(
      std::span<const std::byte>(tx_bid.data() + ack_consumed,
                                  written - ack_consumed),
      fill_msg));
  ASSERT_EQ(static_cast<MsgType>(fill_msg.header.msg_type),
            MsgType::kFillReport);

  FillReport fill;
  ASSERT_TRUE(deserialize_fill_report(fill_msg.payload, fill));
  EXPECT_EQ(fill.client_order_id, bid.client_order_id);
  EXPECT_EQ(fill.fill_price, 100'000);
  EXPECT_EQ(fill.fill_qty, 3U);
  EXPECT_EQ(fill.remaining_qty, 0U); // fully filled
}

// ---------------------------------------------------------------------------
// Invalid symbol_id — reject
// ---------------------------------------------------------------------------

TEST_F(OrderGatewayTest, InvalidSymbolIdRejectsNewOrder) {
  const auto session = gateway_.on_client_connect();

  const NewOrder req{
      .client_order_id = 1,
      .symbol_id = 99, // not registered
      .side = algo::Side::kBid,
      .price = 100'000,
      .qty = 1,
      .send_ts = 0,
  };
  const auto frame = pack_new_order(req);
  ASSERT_GT(frame.len, 0U);

  std::array<std::byte, kGatewayTxCap> tx{};
  const auto written = gateway_.on_message(
      session,
      std::span<const std::byte>(frame.bytes.data(), frame.len), tx);
  ASSERT_GT(written, 0U);

  net::ParsedMessageView parsed;
  ASSERT_TRUE(
      parse_tlv(std::span<const std::byte>(tx.data(), written), parsed));
  EXPECT_EQ(static_cast<MsgType>(parsed.header.msg_type),
            MsgType::kOrderReject);
}

TEST_F(OrderGatewayTest, InvalidSymbolIdRejectsCancelOrder) {
  const auto session = gateway_.on_client_connect();

  const CancelOrder cancel{
      .client_order_id = 1,
      .symbol_id = 99,
      .send_ts = 0,
  };
  const auto frame = pack_cancel_order(cancel);
  ASSERT_GT(frame.len, 0U);

  std::array<std::byte, 512> tx{};
  const auto written = gateway_.on_message(
      session,
      std::span<const std::byte>(frame.bytes.data(), frame.len), tx);
  ASSERT_GT(written, 0U);

  net::ParsedMessageView parsed;
  ASSERT_TRUE(
      parse_tlv(std::span<const std::byte>(tx.data(), written), parsed));
  EXPECT_EQ(static_cast<MsgType>(parsed.header.msg_type),
            MsgType::kCancelReject);
}

// ---------------------------------------------------------------------------
// session_id=0 guard
// ---------------------------------------------------------------------------

TEST_F(OrderGatewayTest, SessionIdZeroReturnsZero) {
  const NewOrder req{
      .client_order_id = 1,
      .symbol_id = 1,
      .side = algo::Side::kBid,
      .price = 100'000,
      .qty = 1,
      .send_ts = 0,
  };
  const auto frame = pack_new_order(req);
  ASSERT_GT(frame.len, 0U);

  std::array<std::byte, kGatewayTxCap> tx{};
  const auto written = gateway_.on_message(
      0, // invalid session
      std::span<const std::byte>(frame.bytes.data(), frame.len), tx);
  EXPECT_EQ(written, 0U);
}

} // namespace
} // namespace mk::app
