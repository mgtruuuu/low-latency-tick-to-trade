/**
 * @file market_data_publisher_test.cpp
 * @brief Unit tests for MarketDataPublisher using mock UDP socket injection.
 *
 * Demonstrates the value of the UdpSendable concept + template parameter:
 * by substituting a MockUdpSocket, we test all publisher logic (sequencing,
 * serialization, A/B feed redundancy) without any real network I/O.
 */

#include "simulated_exchange/market_data_publisher.hpp"

#include "mock_udp_socket.hpp"
#include "shared/protocol.hpp"
#include "shared/protocol_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <span>

namespace mk::app {
namespace {

using test::make_test_addr;
using test::MockUdpSocket;

// Helper: deserialize the n-th sent packet into a MarketDataUpdate.
bool deserialize_packet(const MockUdpSocket &sock, std::size_t index,
                        MarketDataUpdate &out) {
  if (index >= sock.sent_packets.size()) {
    return false;
  }
  const auto &pkt = sock.sent_packets[index];
  return deserialize_market_data(
      std::span<const std::byte>(pkt.data.data(), pkt.data.size()), out);
}

// =============================================================================
// Test fixture
// =============================================================================

class MarketDataPublisherTest : public ::testing::Test {
protected:
  void SetUp() override {
    feed_a_ = make_test_addr("239.255.0.1", 9000);
    feed_b_ = make_test_addr("239.255.0.2", 9001);
  }

  MockUdpSocket sock_;
  sockaddr_in feed_a_{};
  sockaddr_in feed_b_{};
  std::uint64_t seq_num_ = 0;
};

// =============================================================================
// Tests
// =============================================================================

// publish_tick() sends exactly 2 datagrams (bid + ask).
TEST_F(MarketDataPublisherTest, PublishTickSendsTwoPackets) {
  MarketDataPublisher<MockUdpSocket> pub(sock_, /*symbol_id=*/1, seq_num_,
                                         feed_a_);
  pub.publish_tick();

  EXPECT_EQ(sock_.sent_packets.size(), 2U);
}

// Each publish_tick() increments the shared sequence number by 2.
TEST_F(MarketDataPublisherTest, SequenceNumberIncrements) {
  MarketDataPublisher<MockUdpSocket> pub(sock_, /*symbol_id=*/1, seq_num_,
                                         feed_a_);

  pub.publish_tick();
  EXPECT_EQ(seq_num_, 2U);

  pub.publish_tick();
  EXPECT_EQ(seq_num_, 4U);
}

// Sequence numbers are monotonically increasing across packets.
TEST_F(MarketDataPublisherTest, SequenceNumbersAreMonotonic) {
  MarketDataPublisher<MockUdpSocket> pub(sock_, /*symbol_id=*/1, seq_num_,
                                         feed_a_);

  pub.publish_tick();
  pub.publish_tick();

  ASSERT_EQ(sock_.sent_packets.size(), 4U);

  for (std::size_t i = 0; i < sock_.sent_packets.size(); ++i) {
    MarketDataUpdate md;
    ASSERT_TRUE(deserialize_packet(sock_, i, md));
    EXPECT_EQ(md.seq_num, i);
  }
}

// symbol_id is stamped on every outgoing message.
TEST_F(MarketDataPublisherTest, SymbolIdStampedOnEveryMessage) {
  constexpr std::uint32_t kSymbolId = 42;
  MarketDataPublisher<MockUdpSocket> pub(sock_, kSymbolId, seq_num_, feed_a_);

  pub.publish_tick();

  for (std::size_t i = 0; i < sock_.sent_packets.size(); ++i) {
    MarketDataUpdate md;
    ASSERT_TRUE(deserialize_packet(sock_, i, md));
    EXPECT_EQ(md.symbol_id, kSymbolId);
  }
}

// publish_tick() produces one bid and one ask update.
TEST_F(MarketDataPublisherTest, PublishTickProducesBidAndAsk) {
  MarketDataPublisher<MockUdpSocket> pub(sock_, /*symbol_id=*/1, seq_num_,
                                         feed_a_);

  pub.publish_tick();

  ASSERT_EQ(sock_.sent_packets.size(), 2U);

  MarketDataUpdate md0;
  MarketDataUpdate md1;
  ASSERT_TRUE(deserialize_packet(sock_, 0, md0));
  ASSERT_TRUE(deserialize_packet(sock_, 1, md1));

  EXPECT_EQ(md0.side, algo::Side::kBid);
  EXPECT_EQ(md1.side, algo::Side::kAsk);
}

// Prices and quantities are positive and non-zero.
TEST_F(MarketDataPublisherTest, PricesAndQuantitiesArePositive) {
  MarketDataPublisher<MockUdpSocket> pub(sock_, /*symbol_id=*/1, seq_num_,
                                         feed_a_);

  pub.publish_tick();

  for (std::size_t i = 0; i < sock_.sent_packets.size(); ++i) {
    MarketDataUpdate md;
    ASSERT_TRUE(deserialize_packet(sock_, i, md));
    EXPECT_GT(md.price, 0);
    EXPECT_GT(md.qty, 0U);
  }
}

// Bid price < ask price (spread is positive).
TEST_F(MarketDataPublisherTest, BidPriceLessThanAskPrice) {
  MarketDataPublisher<MockUdpSocket> pub(sock_, /*symbol_id=*/1, seq_num_,
                                         feed_a_);

  pub.publish_tick();

  ASSERT_EQ(sock_.sent_packets.size(), 2U);

  MarketDataUpdate bid;
  MarketDataUpdate ask;
  ASSERT_TRUE(deserialize_packet(sock_, 0, bid));
  ASSERT_TRUE(deserialize_packet(sock_, 1, ask));

  EXPECT_LT(bid.price, ask.price);
}

// Wire size of each packet is exactly kMarketDataWireSize (36 bytes).
TEST_F(MarketDataPublisherTest, PacketSizeIsExactlyWireSize) {
  MarketDataPublisher<MockUdpSocket> pub(sock_, /*symbol_id=*/1, seq_num_,
                                         feed_a_);

  pub.publish_tick();

  for (const auto &pkt : sock_.sent_packets) {
    EXPECT_EQ(pkt.data.size(), kMarketDataWireSize);
  }
}

// A/B feed: when feed_b is provided, each publish sends to both destinations.
TEST_F(MarketDataPublisherTest, ABFeedSendsToBothDestinations) {
  MarketDataPublisher<MockUdpSocket> pub(sock_, /*symbol_id=*/1, seq_num_,
                                         feed_a_, &feed_b_);

  pub.publish_tick();

  // 2 updates (bid + ask) x 2 feeds (A + B) = 4 packets.
  ASSERT_EQ(sock_.sent_packets.size(), 4U);

  // Packets 0,1 = Feed A bid, Feed B bid.
  // Packets 2,3 = Feed A ask, Feed B ask.
  EXPECT_EQ(sock_.sent_packets[0].dest.sin_port, htons(9000));
  EXPECT_EQ(sock_.sent_packets[1].dest.sin_port, htons(9001));
  EXPECT_EQ(sock_.sent_packets[2].dest.sin_port, htons(9000));
  EXPECT_EQ(sock_.sent_packets[3].dest.sin_port, htons(9001));
}

// A/B feed: Feed A and Feed B carry identical data (same seq_num, side, etc).
TEST_F(MarketDataPublisherTest, ABFeedDataIsIdentical) {
  MarketDataPublisher<MockUdpSocket> pub(sock_, /*symbol_id=*/1, seq_num_,
                                         feed_a_, &feed_b_);

  pub.publish_tick();

  ASSERT_EQ(sock_.sent_packets.size(), 4U);

  // Feed A bid (packet 0) and Feed B bid (packet 1) have identical bytes.
  EXPECT_EQ(sock_.sent_packets[0].data, sock_.sent_packets[1].data);

  // Feed A ask (packet 2) and Feed B ask (packet 3) have identical bytes.
  EXPECT_EQ(sock_.sent_packets[2].data, sock_.sent_packets[3].data);
}

// No Feed B: only Feed A packets are sent.
TEST_F(MarketDataPublisherTest, NoFeedBSendsOnlyFeedA) {
  MarketDataPublisher<MockUdpSocket> pub(sock_, /*symbol_id=*/1, seq_num_,
                                         feed_a_, /*feed_b_dest=*/nullptr);

  pub.publish_tick();

  EXPECT_EQ(sock_.sent_packets.size(), 2U);

  for (const auto &pkt : sock_.sent_packets) {
    EXPECT_EQ(pkt.dest.sin_port, htons(9000));
  }
}

// Multiple publishers sharing the same seq_num produce a unified stream.
TEST_F(MarketDataPublisherTest, SharedSequenceAcrossPublishers) {
  constexpr std::uint32_t kSymA = 1;
  constexpr std::uint32_t kSymB = 2;

  MarketDataPublisher<MockUdpSocket> pub_a(sock_, kSymA, seq_num_, feed_a_);
  MarketDataPublisher<MockUdpSocket> pub_b(sock_, kSymB, seq_num_, feed_a_);

  pub_a.publish_tick(); // seq 0, 1
  pub_b.publish_tick(); // seq 2, 3

  EXPECT_EQ(seq_num_, 4U);

  ASSERT_EQ(sock_.sent_packets.size(), 4U);

  MarketDataUpdate md0;
  MarketDataUpdate md1;
  MarketDataUpdate md2;
  MarketDataUpdate md3;
  ASSERT_TRUE(deserialize_packet(sock_, 0, md0));
  ASSERT_TRUE(deserialize_packet(sock_, 1, md1));
  ASSERT_TRUE(deserialize_packet(sock_, 2, md2));
  ASSERT_TRUE(deserialize_packet(sock_, 3, md3));

  EXPECT_EQ(md0.symbol_id, kSymA);
  EXPECT_EQ(md1.symbol_id, kSymA);
  EXPECT_EQ(md2.symbol_id, kSymB);
  EXPECT_EQ(md3.symbol_id, kSymB);

  EXPECT_EQ(md0.seq_num, 0U);
  EXPECT_EQ(md1.seq_num, 1U);
  EXPECT_EQ(md2.seq_num, 2U);
  EXPECT_EQ(md3.seq_num, 3U);
}

// publish_update() with explicit side/price/qty works correctly.
TEST_F(MarketDataPublisherTest, PublishUpdateExplicit) {
  MarketDataPublisher<MockUdpSocket> pub(sock_, /*symbol_id=*/7, seq_num_,
                                         feed_a_);

  pub.publish_update(algo::Side::kBid, 999'500, 200);

  ASSERT_EQ(sock_.sent_packets.size(), 1U);

  MarketDataUpdate md;
  ASSERT_TRUE(deserialize_packet(sock_, 0, md));

  EXPECT_EQ(md.seq_num, 0U);
  EXPECT_EQ(md.symbol_id, 7U);
  EXPECT_EQ(md.side, algo::Side::kBid);
  EXPECT_EQ(md.price, 999'500);
  EXPECT_EQ(md.qty, 200U);
  EXPECT_GT(md.exchange_ts, 0);
}

// publish_trade() sends 1 datagram with kTrade msg_type.
TEST_F(MarketDataPublisherTest, PublishTradeSendsTradeType) {
  MarketDataPublisher<MockUdpSocket> pub(sock_, /*symbol_id=*/1, seq_num_,
                                         feed_a_);

  pub.publish_trade(algo::Side::kBid, 1'050'000, 100);

  // 1 Trade datagram (no synthetic opposite-side BBO).
  ASSERT_EQ(sock_.sent_packets.size(), 1U);
  EXPECT_EQ(seq_num_, 1U);

  // Verify kTrade msg_type on the wire.
  MarketDataUpdate md{};
  const auto &pkt = sock_.sent_packets[0].data;
  ASSERT_TRUE(deserialize_market_data(
      std::span<const std::byte>(pkt.data(), pkt.size()), md));
  EXPECT_EQ(md.md_msg_type, MdMsgType::kTrade);
  EXPECT_EQ(md.side, algo::Side::kBid);
  EXPECT_EQ(md.price, 1'050'000);
  EXPECT_EQ(md.qty, 100U);
}

// Send failure does not crash and seq_num still increments.
TEST_F(MarketDataPublisherTest, SendFailureDoesNotCrash) {
  MarketDataPublisher<MockUdpSocket> pub(sock_, /*symbol_id=*/1, seq_num_,
                                         feed_a_);

  sock_.fail_sends = true;
  pub.publish_tick();

  // seq_num still incremented (message was serialized, send just failed).
  EXPECT_EQ(seq_num_, 2U);

  // No packets recorded (mock returned error).
  EXPECT_EQ(sock_.sent_packets.size(), 0U);
}

// Accessor methods return correct values.
TEST_F(MarketDataPublisherTest, Accessors) {
  constexpr std::uint32_t kSymbolId = 99;
  MarketDataPublisher<MockUdpSocket> pub(sock_, kSymbolId, seq_num_, feed_a_);

  EXPECT_EQ(pub.symbol_id(), kSymbolId);
  EXPECT_EQ(pub.seq_num(), 0U);
  EXPECT_GT(pub.mid_price(), 0);

  pub.publish_tick();
  EXPECT_EQ(pub.seq_num(), 2U);
}

} // namespace
} // namespace mk::app
