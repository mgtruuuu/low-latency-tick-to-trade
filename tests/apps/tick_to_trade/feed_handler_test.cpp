#include "tick_to_trade/feed_handler.hpp"

#include "shared/protocol.hpp"
#include "shared/protocol_codec.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

// Serialize a MarketDataUpdate into a wire-format buffer for on_udp_data().
auto make_md_buf(std::uint64_t seq, std::uint32_t symbol_id,
                 mk::algo::Side side, mk::algo::Price price, mk::algo::Qty qty,
                 std::int64_t ts = 0) {
  mk::app::MarketDataUpdate md{};
  md.seq_num = seq;
  md.symbol_id = symbol_id;
  md.side = side;
  md.price = price;
  md.qty = qty;
  md.exchange_ts = ts;

  std::array<std::byte, mk::app::kMarketDataWireSize> buf{};
  auto written = mk::app::serialize_market_data(buf, md);
  EXPECT_EQ(written, mk::app::kMarketDataWireSize);
  return buf;
}

class FeedHandlerTest : public ::testing::Test {
protected:
  mk::app::FeedHandler handler_;
};

TEST_F(FeedHandlerTest, ParseValid) {
  auto buf = make_md_buf(1, 1, mk::algo::Side::kBid, 10000, 100, 999);

  mk::app::MarketDataUpdate out{};
  ASSERT_TRUE(handler_.on_udp_data(reinterpret_cast<const char *>(buf.data()),
                                   buf.size(), out));

  EXPECT_EQ(out.seq_num, 1U);
  EXPECT_EQ(out.symbol_id, 1U);
  EXPECT_EQ(out.side, mk::algo::Side::kBid);
  EXPECT_EQ(out.price, 10000);
  EXPECT_EQ(out.qty, 100U);
  EXPECT_EQ(out.exchange_ts, 999);
}

TEST_F(FeedHandlerTest, ParseTooShort) {
  std::array<std::byte, 10> buf{};
  mk::app::MarketDataUpdate out{};

  EXPECT_FALSE(handler_.on_udp_data(reinterpret_cast<const char *>(buf.data()),
                                    buf.size(), out));
  EXPECT_EQ(handler_.parse_errors(), 1U);
}

TEST_F(FeedHandlerTest, SequentialSequence) {
  mk::app::MarketDataUpdate out{};

  for (std::uint64_t seq = 1; seq <= 3; ++seq) {
    auto buf = make_md_buf(seq, 1, mk::algo::Side::kBid, 10000, 100);
    EXPECT_TRUE(handler_.on_udp_data(reinterpret_cast<const char *>(buf.data()),
                                     buf.size(), out));
  }

  EXPECT_EQ(handler_.expected_seq(), 4U);
  EXPECT_EQ(handler_.gap_count(), 0U);
  EXPECT_EQ(handler_.duplicate_count(), 0U);
  EXPECT_EQ(handler_.total_updates(), 3U);
}

TEST_F(FeedHandlerTest, GapDetection) {
  mk::app::MarketDataUpdate out{};

  // Seq 1 — normal.
  auto buf1 = make_md_buf(1, 1, mk::algo::Side::kBid, 10000, 100);
  ASSERT_TRUE(handler_.on_udp_data(reinterpret_cast<const char *>(buf1.data()),
                                   buf1.size(), out));

  // Seq 5 — gap of 3 (missed seq 2, 3, 4).
  auto buf5 = make_md_buf(5, 1, mk::algo::Side::kBid, 10000, 100);
  ASSERT_TRUE(handler_.on_udp_data(reinterpret_cast<const char *>(buf5.data()),
                                   buf5.size(), out));

  EXPECT_EQ(handler_.gap_count(), 3U);
  EXPECT_EQ(handler_.expected_seq(), 6U);
  EXPECT_EQ(handler_.total_updates(), 2U);
}

TEST_F(FeedHandlerTest, DuplicateRejection) {
  mk::app::MarketDataUpdate out{};

  auto buf1 = make_md_buf(1, 1, mk::algo::Side::kBid, 10000, 100);
  auto buf2 = make_md_buf(2, 1, mk::algo::Side::kBid, 10000, 100);

  ASSERT_TRUE(handler_.on_udp_data(reinterpret_cast<const char *>(buf1.data()),
                                   buf1.size(), out));
  ASSERT_TRUE(handler_.on_udp_data(reinterpret_cast<const char *>(buf2.data()),
                                   buf2.size(), out));

  // Re-send seq 1 — duplicate.
  EXPECT_FALSE(handler_.on_udp_data(reinterpret_cast<const char *>(buf1.data()),
                                    buf1.size(), out));

  EXPECT_EQ(handler_.duplicate_count(), 1U);
  EXPECT_EQ(handler_.total_updates(), 2U);
}

TEST_F(FeedHandlerTest, CounterConsistency) {
  mk::app::MarketDataUpdate out{};

  // 5 valid, 2 invalid (too short), 1 duplicate.
  for (std::uint64_t seq = 1; seq <= 5; ++seq) {
    auto buf = make_md_buf(seq, 1, mk::algo::Side::kBid, 10000, 100);
    EXPECT_TRUE(handler_.on_udp_data(reinterpret_cast<const char *>(buf.data()),
                                     buf.size(), out));
  }

  std::array<std::byte, 5> short_buf{};
  EXPECT_FALSE(handler_.on_udp_data(
      reinterpret_cast<const char *>(short_buf.data()), short_buf.size(), out));
  EXPECT_FALSE(handler_.on_udp_data(
      reinterpret_cast<const char *>(short_buf.data()), short_buf.size(), out));

  // Duplicate of seq 3.
  auto dup = make_md_buf(3, 1, mk::algo::Side::kBid, 10000, 100);
  EXPECT_FALSE(handler_.on_udp_data(reinterpret_cast<const char *>(dup.data()),
                                    dup.size(), out));

  EXPECT_EQ(handler_.total_updates(), 5U);
  EXPECT_EQ(handler_.parse_errors(), 2U);
  EXPECT_EQ(handler_.duplicate_count(), 1U);
}

// ---------------------------------------------------------------------------
// Late join: first packet sets baseline without gap warning
// ---------------------------------------------------------------------------

TEST_F(FeedHandlerTest, FirstPacketSetsBaselineWithoutGap) {
  // Simulate late multicast join — first packet has seq=7025, not 1.
  auto buf = make_md_buf(7025, 1, mk::algo::Side::kBid, 100, 10);
  mk::app::MarketDataUpdate out{};

  ASSERT_TRUE(handler_.on_udp_data(reinterpret_cast<const char *>(buf.data()),
                                   buf.size(), out));
  EXPECT_EQ(out.seq_num, 7025U);
  EXPECT_EQ(handler_.expected_seq(), 7026U);
  EXPECT_EQ(handler_.gap_count(), 0U); // no false gap
  EXPECT_EQ(handler_.total_updates(), 1U);
}

TEST_F(FeedHandlerTest, LateJoinSecondPacketGapDetectedFromBaseline) {
  // First packet: seq=7025 → baseline.
  auto buf1 = make_md_buf(7025, 1, mk::algo::Side::kBid, 100, 10);
  mk::app::MarketDataUpdate out{};
  ASSERT_TRUE(handler_.on_udp_data(reinterpret_cast<const char *>(buf1.data()),
                                   buf1.size(), out));

  // Second packet: seq=7027 → gap of 1 (7026 missing).
  auto buf2 = make_md_buf(7027, 1, mk::algo::Side::kAsk, 101, 20);
  ASSERT_TRUE(handler_.on_udp_data(reinterpret_cast<const char *>(buf2.data()),
                                   buf2.size(), out));
  EXPECT_EQ(handler_.expected_seq(), 7028U);
  EXPECT_EQ(handler_.gap_count(), 1U); // real gap detected
  EXPECT_EQ(handler_.total_updates(), 2U);
}

} // namespace
