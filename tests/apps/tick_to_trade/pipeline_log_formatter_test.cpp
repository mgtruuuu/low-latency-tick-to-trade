/**
 * @file pipeline_log_formatter_test.cpp
 * @brief Regression tests for PipelineLogFormatter text output.
 *
 * Verifies that each LogEntry event type produces the expected text fields
 * after formatting. Catches string table mismatches, field omissions, and
 * formatting regressions that the generic AsyncDrainLoop tests do not cover.
 */

#include "tick_to_trade/pipeline_log_entry.hpp"
#include "tick_to_trade/pipeline_log_formatter.hpp"

#include "sys/nano_clock.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>

namespace mk::app {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Format a LogEntry to text and return as std::string for easy assertions.
std::string format(const LogEntry &entry) {
  std::array<char, 512> buf{};
  const auto cal = sys::TscCalibration::calibrate();
  const PipelineLogFormatter formatter;
  const auto len = formatter(entry, cal, buf);
  return {buf.data(), len};
}

// ---------------------------------------------------------------------------
// Latency
// ---------------------------------------------------------------------------

TEST(PipelineLogFormatterTest, LatencyEntry) {
  LogEntry entry{};
  entry.tsc_timestamp = 1000;
  entry.thread_id = kThreadIdMd;
  entry.level = LogLevel::kInfo;
  entry.event_type = LogEventType::kLatency;
  entry.latency.stage = LatencyStage::kFeedParse;
  entry.latency.cycles = 142;

  const auto text = format(entry);
  EXPECT_NE(text.find("MD"), std::string::npos);
  EXPECT_NE(text.find("INFO"), std::string::npos);
  EXPECT_NE(text.find("LATENCY"), std::string::npos);
  EXPECT_NE(text.find("stage=FeedParse"), std::string::npos);
  EXPECT_NE(text.find("cycles=142"), std::string::npos);
  EXPECT_NE(text.find("ns="), std::string::npos);
}

// ---------------------------------------------------------------------------
// Order
// ---------------------------------------------------------------------------

TEST(PipelineLogFormatterTest, OrderEntry) {
  LogEntry entry{};
  entry.tsc_timestamp = 2000;
  entry.thread_id = kThreadIdStrategy;
  entry.level = LogLevel::kInfo;
  entry.event_type = LogEventType::kOrder;
  entry.order.sub_type = OrderEvent::kNewOrder;
  entry.order.symbol_id = 1;
  entry.order.side = 0; // Bid
  entry.order.price = 10050;
  entry.order.qty = 10;
  entry.order.client_order_id = 42;

  const auto text = format(entry);
  EXPECT_NE(text.find("STRAT"), std::string::npos);
  EXPECT_NE(text.find("ORDER"), std::string::npos);
  EXPECT_NE(text.find("event=NewOrder"), std::string::npos);
  EXPECT_NE(text.find("sym=1"), std::string::npos);
  EXPECT_NE(text.find("side=Bid"), std::string::npos);
  EXPECT_NE(text.find("price=10050"), std::string::npos);
  EXPECT_NE(text.find("qty=10"), std::string::npos);
  EXPECT_NE(text.find("oid=42"), std::string::npos);
}

TEST(PipelineLogFormatterTest, OrderEntryOmitsZeroOptionals) {
  LogEntry entry{};
  entry.tsc_timestamp = 2500;
  entry.thread_id = kThreadIdStrategy;
  entry.level = LogLevel::kInfo;
  entry.event_type = LogEventType::kOrder;
  entry.order.sub_type = OrderEvent::kNewOrder;
  entry.order.symbol_id = 1;
  entry.order.client_order_id = 7;
  // exchange_order_id = 0, remaining_qty = 0 (defaults)

  const auto text = format(entry);
  EXPECT_EQ(text.find("xoid="), std::string::npos)
      << "xoid should be omitted when 0";
  EXPECT_EQ(text.find("rem="), std::string::npos)
      << "rem should be omitted when 0";
}

TEST(PipelineLogFormatterTest, OrderEntryWithOptionalFields) {
  LogEntry entry{};
  entry.tsc_timestamp = 3000;
  entry.thread_id = kThreadIdStrategy;
  entry.level = LogLevel::kInfo;
  entry.event_type = LogEventType::kOrder;
  entry.order.sub_type = OrderEvent::kFill;
  entry.order.symbol_id = 2;
  entry.order.side = 1; // Ask
  entry.order.price = 20100;
  entry.order.qty = 50;
  entry.order.client_order_id = 100;
  entry.order.exchange_order_id = 99;
  entry.order.remaining_qty = 5;

  const auto text = format(entry);
  EXPECT_NE(text.find("event=Fill"), std::string::npos);
  EXPECT_NE(text.find("side=Ask"), std::string::npos);
  EXPECT_NE(text.find("xoid=99"), std::string::npos);
  EXPECT_NE(text.find("rem=5"), std::string::npos);
}

// ---------------------------------------------------------------------------
// MarketData
// ---------------------------------------------------------------------------

TEST(PipelineLogFormatterTest, MarketDataOmitsZeroGap) {
  LogEntry entry{};
  entry.tsc_timestamp = 3500;
  entry.thread_id = kThreadIdMd;
  entry.level = LogLevel::kInfo;
  entry.event_type = LogEventType::kMarketData;
  entry.market_data.seq_num = 50;
  entry.market_data.symbol_id = 1;
  // gap_size = 0 (default)

  const auto text = format(entry);
  EXPECT_EQ(text.find("gap="), std::string::npos)
      << "gap should be omitted when 0";
}

TEST(PipelineLogFormatterTest, MarketDataEntry) {
  LogEntry entry{};
  entry.tsc_timestamp = 4000;
  entry.thread_id = kThreadIdMd;
  entry.level = LogLevel::kInfo;
  entry.event_type = LogEventType::kMarketData;
  entry.market_data.seq_num = 100;
  entry.market_data.symbol_id = 2;
  entry.market_data.side = 1; // Ask
  entry.market_data.price = 20100;
  entry.market_data.qty = 50;
  entry.market_data.gap_size = 3;

  const auto text = format(entry);
  EXPECT_NE(text.find("MKTDATA"), std::string::npos);
  EXPECT_NE(text.find("seq=100"), std::string::npos);
  EXPECT_NE(text.find("sym=2"), std::string::npos);
  EXPECT_NE(text.find("side=Ask"), std::string::npos);
  EXPECT_NE(text.find("price=20100"), std::string::npos);
  EXPECT_NE(text.find("qty=50"), std::string::npos);
  EXPECT_NE(text.find("gap=3"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

TEST(PipelineLogFormatterTest, ConnectionOmitsZeroOptionals) {
  LogEntry entry{};
  entry.tsc_timestamp = 4500;
  entry.thread_id = kThreadIdStrategy;
  entry.level = LogLevel::kInfo;
  entry.event_type = LogEventType::kConnection;
  entry.connection.sub_type = ConnectionEvent::kConnect;
  // attempt = 0, rtt_ns = 0 (defaults)

  const auto text = format(entry);
  EXPECT_EQ(text.find("attempt="), std::string::npos)
      << "attempt should be omitted when 0";
  EXPECT_EQ(text.find("rtt_ns="), std::string::npos)
      << "rtt_ns should be omitted when 0";
}

TEST(PipelineLogFormatterTest, ConnectionEntryWithRtt) {
  LogEntry entry{};
  entry.tsc_timestamp = 4600;
  entry.thread_id = kThreadIdStrategy;
  entry.level = LogLevel::kInfo;
  entry.event_type = LogEventType::kConnection;
  entry.connection.sub_type = ConnectionEvent::kHeartbeatRecv;
  entry.connection.rtt_ns = 12345;

  const auto text = format(entry);
  EXPECT_NE(text.find("event=HbRecv"), std::string::npos);
  EXPECT_NE(text.find("rtt_ns=12345"), std::string::npos);
}

TEST(PipelineLogFormatterTest, ConnectionEntry) {
  LogEntry entry{};
  entry.tsc_timestamp = 5000;
  entry.thread_id = kThreadIdStrategy;
  entry.level = LogLevel::kWarn;
  entry.event_type = LogEventType::kConnection;
  entry.connection.sub_type = ConnectionEvent::kDisconnect;
  entry.connection.attempt = 1;

  const auto text = format(entry);
  EXPECT_NE(text.find("STRAT"), std::string::npos);
  EXPECT_NE(text.find("WARN"), std::string::npos);
  EXPECT_NE(text.find("CONN"), std::string::npos);
  EXPECT_NE(text.find("event=Disconnect"), std::string::npos);
  EXPECT_NE(text.find("attempt=1"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

TEST(PipelineLogFormatterTest, TextEntry) {
  LogEntry entry{};
  entry.tsc_timestamp = 6000;
  entry.thread_id = kThreadIdMain;
  entry.level = LogLevel::kInfo;
  entry.event_type = LogEventType::kText;
  std::strncpy(entry.text.msg, "Pipeline starting", sizeof(entry.text.msg) - 1);

  const auto text = format(entry);
  EXPECT_NE(text.find("MAIN"), std::string::npos);
  EXPECT_NE(text.find("TEXT"), std::string::npos);
  EXPECT_NE(text.find("Pipeline starting"), std::string::npos);
}

// ---------------------------------------------------------------------------
// All latency stages present in string table
// ---------------------------------------------------------------------------

TEST(PipelineLogFormatterTest, AllLatencyStages) {
  constexpr std::array kExpected = {"UdpRecv",  "FeedParse", "QueueHop",
                                    "Strategy", "OrderSend", "T2T"};
  static_assert(kExpected.size() ==
                    static_cast<std::size_t>(LatencyStage::kTickToTrade) + 1,
                "kExpected size must match LatencyStage enum count");
  for (int i = 0; i <= static_cast<int>(LatencyStage::kTickToTrade); ++i) {
    LogEntry entry{};
    entry.tsc_timestamp = 1000;
    entry.thread_id = kThreadIdMd;
    entry.event_type = LogEventType::kLatency;
    entry.latency.stage = static_cast<LatencyStage>(i);
    entry.latency.cycles = 1;

    const auto text = format(entry);
    EXPECT_NE(text.find(kExpected[i]), std::string::npos)
        << "Missing stage name for index " << i;
  }
}

// ---------------------------------------------------------------------------
// All order event sub-types present in string table
// ---------------------------------------------------------------------------

TEST(PipelineLogFormatterTest, AllOrderEvents) {
  constexpr std::array kExpected = {
      "NewOrder",  "OrderAck",     "OrderReject", "Fill",      "CancelSent",
      "CancelAck", "CancelReject", "ModifySent",  "ModifyAck", "ModifyReject"};
  static_assert(kExpected.size() ==
                    static_cast<std::size_t>(OrderEvent::kModifyReject) + 1,
                "kExpected size must match OrderEvent enum count");
  for (int i = 0; i <= static_cast<int>(OrderEvent::kModifyReject); ++i) {
    LogEntry entry{};
    entry.tsc_timestamp = 1000;
    entry.thread_id = kThreadIdStrategy;
    entry.event_type = LogEventType::kOrder;
    entry.order.sub_type = static_cast<OrderEvent>(i);
    entry.order.symbol_id = 1;

    const auto text = format(entry);
    EXPECT_NE(text.find(kExpected[i]), std::string::npos)
        << "Missing order event name for index " << i;
  }
}

// ---------------------------------------------------------------------------
// All connection events present in string table
// ---------------------------------------------------------------------------

TEST(PipelineLogFormatterTest, AllConnectionEvents) {
  constexpr std::array kExpected = {"Connect", "Disconnect", "HbSent", "HbRecv",
                                    "Reconnect"};
  static_assert(
      kExpected.size() ==
          static_cast<std::size_t>(ConnectionEvent::kReconnectAttempt) + 1,
      "kExpected size must match ConnectionEvent enum count");
  for (int i = 0; i <= static_cast<int>(ConnectionEvent::kReconnectAttempt);
       ++i) {
    LogEntry entry{};
    entry.tsc_timestamp = 1000;
    entry.thread_id = kThreadIdStrategy;
    entry.event_type = LogEventType::kConnection;
    entry.connection.sub_type = static_cast<ConnectionEvent>(i);

    const auto text = format(entry);
    EXPECT_NE(text.find(kExpected[i]), std::string::npos)
        << "Missing connection event name for index " << i;
  }
}

} // namespace
} // namespace mk::app
