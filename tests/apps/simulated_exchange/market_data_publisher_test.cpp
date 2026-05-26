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

#include "net/message_codec.hpp"

#include <array>
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

// Wire size of each packet is exactly kMarketDataWireSize (34 bytes).
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

// =============================================================================
// Codec contract tests — guard the 34-byte UDP wire layout against
// silent regressions (offset shifts, padding reintroduction, size drift).
// =============================================================================

// Wire layout is exactly 34 bytes: serializing into a 34-byte buffer succeeds
// and writes the full size; the constant is wired to that expected value.
TEST(MarketDataCodecGoldenTest, WireSizeIsExactly34Bytes) {
  static_assert(kMarketDataWireSize == 34,
                "UDP MarketData wire size is fixed at 34 bytes");

  const MarketDataUpdate md{};
  std::array<std::byte, kMarketDataWireSize> buf{};
  const std::size_t written =
      serialize_market_data(std::span<std::byte>(buf), md);
  EXPECT_EQ(written, kMarketDataWireSize);
  EXPECT_EQ(written, 34U);
}

// Golden offsets: round-trip a known-distinct value through each field and
// verify it lands at the documented byte position on the wire. This catches
// any future offset shift (e.g., re-adding padding) that would still pass
// the round-trip-only tests.
TEST(MarketDataCodecGoldenTest, FieldsLandAtDocumentedOffsets) {
  MarketDataUpdate md{};
  md.seq_num = 0x0102030405060708ULL;     // offset 0,  8 bytes BE
  md.symbol_id = 0x090A0B0CU;             // offset 8,  4 bytes BE
  md.md_msg_type = MdMsgType::kTrade;     // offset 12, 1 byte (= 1)
  md.side = algo::Side::kAsk;             // offset 13, 1 byte (= 1)
  md.price = 0x1112131415161718LL;        // offset 14, 8 bytes BE
  md.qty = 0x191A1B1CU;                   // offset 22, 4 bytes BE
  md.exchange_ts = 0x2122232425262728LL;  // offset 26, 8 bytes BE

  std::array<std::byte, kMarketDataWireSize> buf{};
  ASSERT_EQ(serialize_market_data(std::span<std::byte>(buf), md),
            kMarketDataWireSize);

  // Spot-check the leading byte of each big-endian field — if any offset
  // shifts by even 1 byte, these checks fire.
  EXPECT_EQ(std::to_integer<std::uint8_t>(buf[0]), 0x01U);  // seq_num MSB
  EXPECT_EQ(std::to_integer<std::uint8_t>(buf[8]), 0x09U);  // symbol_id MSB
  EXPECT_EQ(std::to_integer<std::uint8_t>(buf[12]), 0x01U); // md_msg_type
  EXPECT_EQ(std::to_integer<std::uint8_t>(buf[13]), 0x01U); // side
  EXPECT_EQ(std::to_integer<std::uint8_t>(buf[14]), 0x11U); // price MSB
  EXPECT_EQ(std::to_integer<std::uint8_t>(buf[22]), 0x19U); // qty MSB
  EXPECT_EQ(std::to_integer<std::uint8_t>(buf[26]), 0x21U); // exchange_ts MSB

  // Round-trip recovers every field bit-for-bit.
  MarketDataUpdate out{};
  ASSERT_TRUE(
      deserialize_market_data(std::span<const std::byte>(buf), out));
  EXPECT_EQ(out.seq_num, md.seq_num);
  EXPECT_EQ(out.symbol_id, md.symbol_id);
  EXPECT_EQ(out.md_msg_type, md.md_msg_type);
  EXPECT_EQ(out.side, md.side);
  EXPECT_EQ(out.price, md.price);
  EXPECT_EQ(out.qty, md.qty);
  EXPECT_EQ(out.exchange_ts, md.exchange_ts);
}

// Protocol-version freeze: any change to kProtocolVersion is intentional
// and must be paired with a wire-format change + receiver-side rejection
// logic. If this assertion fires, audit the wire contract (MsgType
// numeric IDs, payload layouts) and the verify_protocol_version() call
// sites in exchange_gateway_main.cpp + strategy_thread.hpp before
// updating the expected value.
TEST(ProtocolVersionTest, CurrentVersionIsFrozen) {
  EXPECT_EQ(kProtocolVersion, 2);
}

// verify_protocol_version() accepts only the current build's version.
TEST(ProtocolVersionTest, VerifyAcceptsCurrentRejectsOthers) {
  EXPECT_TRUE(verify_protocol_version(kProtocolVersion));
  // Older versions (e.g., a v1 peer that pre-dates the UDP MarketData
  // 36B->34B compaction and the receiver-side version enforcement
  // added in v2) must be rejected so the mismatch surfaces loudly
  // instead of letting a 36B legacy datagram or a v1 TCP frame slip
  // past at the framing boundary.
  EXPECT_FALSE(verify_protocol_version(0));
  EXPECT_FALSE(verify_protocol_version(1));
  // Newer-than-current also rejected — the receiver has no way to
  // interpret a future format.
  EXPECT_FALSE(verify_protocol_version(kProtocolVersion + 1));
  EXPECT_FALSE(verify_protocol_version(0xFFFF));
}

// pack_tcp_message stamps the current kProtocolVersion in the header,
// so a freshly packed frame round-trips through verify_protocol_version()
// successfully.
TEST(ProtocolVersionTest, PackedFrameCarriesCurrentVersion) {
  // Pack a minimal NewOrder payload into a TLV frame.
  const NewOrder order{};
  std::array<std::byte, kNewOrderWireSize> payload_buf{};
  const std::size_t payload_len =
      serialize_new_order(std::span<std::byte>(payload_buf), order);
  ASSERT_EQ(payload_len, kNewOrderWireSize);

  std::array<std::byte, net::kMessageHeaderSize + kNewOrderWireSize>
      frame_buf{};
  const std::size_t frame_len = pack_tcp_message(
      std::span<std::byte>(frame_buf), MsgType::kNewOrder,
      std::span<const std::byte>(payload_buf.data(), payload_len));
  ASSERT_GT(frame_len, 0U);

  net::ParsedMessageView msg{};
  ASSERT_TRUE(net::unpack_message(
      std::span<const std::byte>(frame_buf.data(), frame_len), msg));
  EXPECT_EQ(msg.header.version, kProtocolVersion);
  EXPECT_TRUE(verify_protocol_version(msg.header.version));
}

// Strict-equality deserialize: a buffer with the wrong size — including
// a legacy 36-byte datagram from an older codec — must be rejected, not
// silently parsed with shifted offsets.
TEST(MarketDataCodecGoldenTest, DeserializeRejectsWrongSize) {
  MarketDataUpdate out{};

  // Too small: short by one byte.
  std::array<std::byte, kMarketDataWireSize - 1> too_small{};
  EXPECT_FALSE(deserialize_market_data(std::span<const std::byte>(too_small),
                                       out));

  // Too small: empty.
  EXPECT_FALSE(
      deserialize_market_data(std::span<const std::byte>{}, out));

  // Too large by one byte: still rejected. Strict equality also catches
  // the legacy 36-byte layout that would otherwise read price/qty from
  // the wrong offsets.
  std::array<std::byte, kMarketDataWireSize + 1> too_large{};
  EXPECT_FALSE(deserialize_market_data(std::span<const std::byte>(too_large),
                                       out));

  std::array<std::byte, 36> legacy_36b{};
  EXPECT_FALSE(deserialize_market_data(std::span<const std::byte>(legacy_36b),
                                       out));
}

} // namespace
} // namespace mk::app
