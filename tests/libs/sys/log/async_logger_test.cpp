/**
 * @file async_logger_test.cpp
 * @brief Tests for AsyncLogger — SPSC queue draining, text formatting, file I/O.
 */

#include "sys/log/async_logger.hpp"
#include "sys/log/async_log_entry.hpp"
#include "sys/log/log_macros.hpp"
#include "sys/nano_clock.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace mk::sys::log {
namespace {

// Helper: create a unique temporary file path.
std::string make_temp_path(const char *suffix) {
  char tmpl[] = "/tmp/async_logger_test_XXXXXX";
  const int fd = mkstemp(tmpl);
  if (fd >= 0) {
    ::close(fd);
    ::unlink(tmpl); // Remove so AsyncLogger can create fresh.
  }
  return std::string(tmpl) + suffix;
}

// Helper: read entire file contents.
std::string read_file(const std::string &path) {
  const std::ifstream ifs(path);
  std::ostringstream oss;
  oss << ifs.rdbuf();
  return oss.str();
}

// -- LogEntry construction tests --

TEST(LogEntryTest, SizeIs128Bytes) {
  EXPECT_EQ(sizeof(LogEntry), 128);
}

TEST(LogEntryTest, DefaultInitZeroesHeader) {
  const LogEntry entry{};
  EXPECT_EQ(entry.tsc_timestamp, 0);
  EXPECT_EQ(entry.thread_id, 0);
  EXPECT_EQ(entry.level, LogLevel::kInfo);
  EXPECT_EQ(entry.event_type, LogEventType::kText);
}

TEST(LogEntryTest, LatencyStageRoundTrip) {
  LogEntry entry{};
  entry.event_type = LogEventType::kLatency;
  entry.latency.stage = LatencyStage::kFeedParse;
  entry.latency.cycles = 142;
  entry.latency.recv_tsc = 999;

  EXPECT_EQ(entry.latency.stage, LatencyStage::kFeedParse);
  EXPECT_EQ(entry.latency.cycles, 142);
  EXPECT_EQ(entry.latency.recv_tsc, 999);
}

TEST(LogEntryTest, OrderFieldsRoundTrip) {
  LogEntry entry{};
  entry.event_type = LogEventType::kOrder;
  entry.order.sub_type = OrderEvent::kNewOrder;
  entry.order.side = 0; // Bid
  entry.order.symbol_id = 42;
  entry.order.client_order_id = 12345;
  entry.order.price = -5000;
  entry.order.qty = 100;

  EXPECT_EQ(entry.order.sub_type, OrderEvent::kNewOrder);
  EXPECT_EQ(entry.order.side, 0);
  EXPECT_EQ(entry.order.symbol_id, 42);
  EXPECT_EQ(entry.order.client_order_id, 12345);
  EXPECT_EQ(entry.order.price, -5000);
  EXPECT_EQ(entry.order.qty, 100);
}

// -- AsyncLogger integration tests --

class AsyncLoggerTest : public ::testing::Test {
protected:
  void SetUp() override { log_path_ = make_temp_path(".log"); }

  void TearDown() override { ::unlink(log_path_.c_str()); }

  std::string log_path_;
};

TEST_F(AsyncLoggerTest, StartsAndStopsCleanly) {
  AsyncLogger logger(log_path_.c_str(), 256);
  logger.start();
  logger.stop();
  EXPECT_EQ(logger.entries_written(), 0);
}

TEST_F(AsyncLoggerTest, WritesLatencyEntry) {
  AsyncLogger logger(log_path_.c_str(), 256);
  logger.start();

  LogEntry entry{};
  entry.tsc_timestamp = rdtsc();
  entry.thread_id = kThreadIdMd;
  entry.level = LogLevel::kInfo;
  entry.event_type = LogEventType::kLatency;
  entry.latency.stage = LatencyStage::kFeedParse;
  entry.latency.cycles = 142;

  EXPECT_TRUE(logger.md_queue().try_push(entry));

  // Give logger thread time to drain.
  usleep(50'000); // 50ms
  logger.stop();

  EXPECT_EQ(logger.entries_written(), 1);

  auto content = read_file(log_path_);
  EXPECT_NE(content.find("MD"), std::string::npos);
  EXPECT_NE(content.find("INFO"), std::string::npos);
  EXPECT_NE(content.find("LATENCY"), std::string::npos);
  EXPECT_NE(content.find("stage=FeedParse"), std::string::npos);
  EXPECT_NE(content.find("cycles=142"), std::string::npos);
}

TEST_F(AsyncLoggerTest, WritesOrderEntry) {
  AsyncLogger logger(log_path_.c_str(), 256);
  logger.start();

  LogEntry entry{};
  entry.tsc_timestamp = rdtsc();
  entry.thread_id = kThreadIdStrategy;
  entry.level = LogLevel::kInfo;
  entry.event_type = LogEventType::kOrder;
  entry.order.sub_type = OrderEvent::kNewOrder;
  entry.order.symbol_id = 1;
  entry.order.side = 0;
  entry.order.price = 10050;
  entry.order.qty = 10;
  entry.order.client_order_id = 42;

  EXPECT_TRUE(logger.strategy_queue().try_push(entry));

  usleep(50'000);
  logger.stop();

  EXPECT_EQ(logger.entries_written(), 1);

  auto content = read_file(log_path_);
  EXPECT_NE(content.find("STRAT"), std::string::npos);
  EXPECT_NE(content.find("ORDER"), std::string::npos);
  EXPECT_NE(content.find("event=NewOrder"), std::string::npos);
  EXPECT_NE(content.find("sym=1"), std::string::npos);
  EXPECT_NE(content.find("side=Bid"), std::string::npos);
  EXPECT_NE(content.find("price=10050"), std::string::npos);
  EXPECT_NE(content.find("qty=10"), std::string::npos);
  EXPECT_NE(content.find("oid=42"), std::string::npos);
}

TEST_F(AsyncLoggerTest, WritesTextEntry) {
  AsyncLogger logger(log_path_.c_str(), 256);
  logger.start();

  LogEntry entry{};
  entry.tsc_timestamp = rdtsc();
  entry.thread_id = kThreadIdMain;
  entry.level = LogLevel::kInfo;
  entry.event_type = LogEventType::kText;
  std::strncpy(entry.text.msg, "Pipeline starting", sizeof(entry.text.msg) - 1);

  EXPECT_TRUE(logger.md_queue().try_push(entry));

  usleep(50'000);
  logger.stop();

  auto content = read_file(log_path_);
  EXPECT_NE(content.find("MAIN"), std::string::npos);
  EXPECT_NE(content.find("TEXT"), std::string::npos);
  EXPECT_NE(content.find("Pipeline starting"), std::string::npos);
}

TEST_F(AsyncLoggerTest, WritesConnectionEntry) {
  AsyncLogger logger(log_path_.c_str(), 256);
  logger.start();

  LogEntry entry{};
  entry.tsc_timestamp = rdtsc();
  entry.thread_id = kThreadIdStrategy;
  entry.level = LogLevel::kWarn;
  entry.event_type = LogEventType::kConnection;
  entry.connection.sub_type = ConnectionEvent::kDisconnect;
  entry.connection.attempt = 1;

  EXPECT_TRUE(logger.strategy_queue().try_push(entry));

  usleep(50'000);
  logger.stop();

  auto content = read_file(log_path_);
  EXPECT_NE(content.find("STRAT"), std::string::npos);
  EXPECT_NE(content.find("WARN"), std::string::npos);
  EXPECT_NE(content.find("CONN"), std::string::npos);
  EXPECT_NE(content.find("event=Disconnect"), std::string::npos);
  EXPECT_NE(content.find("attempt=1"), std::string::npos);
}

TEST_F(AsyncLoggerTest, WritesMarketDataEntry) {
  AsyncLogger logger(log_path_.c_str(), 256);
  logger.start();

  LogEntry entry{};
  entry.tsc_timestamp = rdtsc();
  entry.thread_id = kThreadIdMd;
  entry.level = LogLevel::kInfo;
  entry.event_type = LogEventType::kMarketData;
  entry.market_data.seq_num = 100;
  entry.market_data.symbol_id = 2;
  entry.market_data.side = 1; // Ask
  entry.market_data.price = 20100;
  entry.market_data.qty = 50;
  entry.market_data.gap_size = 3;

  EXPECT_TRUE(logger.md_queue().try_push(entry));

  usleep(50'000);
  logger.stop();

  auto content = read_file(log_path_);
  EXPECT_NE(content.find("MKTDATA"), std::string::npos);
  EXPECT_NE(content.find("seq=100"), std::string::npos);
  EXPECT_NE(content.find("sym=2"), std::string::npos);
  EXPECT_NE(content.find("side=Ask"), std::string::npos);
  EXPECT_NE(content.find("price=20100"), std::string::npos);
  EXPECT_NE(content.find("qty=50"), std::string::npos);
  EXPECT_NE(content.find("gap=3"), std::string::npos);
}

TEST_F(AsyncLoggerTest, DrainsBothQueues) {
  AsyncLogger logger(log_path_.c_str(), 256);
  logger.start();

  // Push to MD queue.
  LogEntry md_entry{};
  md_entry.tsc_timestamp = rdtsc();
  md_entry.thread_id = kThreadIdMd;
  md_entry.level = LogLevel::kInfo;
  md_entry.event_type = LogEventType::kText;
  std::strncpy(md_entry.text.msg, "from_md", sizeof(md_entry.text.msg) - 1);
  EXPECT_TRUE(logger.md_queue().try_push(md_entry));

  // Push to Strategy queue.
  LogEntry strat_entry{};
  strat_entry.tsc_timestamp = rdtsc();
  strat_entry.thread_id = kThreadIdStrategy;
  strat_entry.level = LogLevel::kInfo;
  strat_entry.event_type = LogEventType::kText;
  std::strncpy(strat_entry.text.msg, "from_strat",
               sizeof(strat_entry.text.msg) - 1);
  EXPECT_TRUE(logger.strategy_queue().try_push(strat_entry));

  usleep(50'000);
  logger.stop();

  EXPECT_EQ(logger.entries_written(), 2);

  auto content = read_file(log_path_);
  EXPECT_NE(content.find("from_md"), std::string::npos);
  EXPECT_NE(content.find("from_strat"), std::string::npos);
}

TEST_F(AsyncLoggerTest, FinalDrainOnStop) {
  // Verify that stop() flushes remaining entries after the stop flag is set.
  AsyncLogger logger(log_path_.c_str(), 256);
  logger.start();

  constexpr int kCount = 50;
  for (int i = 0; i < kCount; ++i) {
    LogEntry entry{};
    entry.tsc_timestamp = rdtsc();
    entry.thread_id = kThreadIdMd;
    entry.level = LogLevel::kInfo;
    entry.event_type = LogEventType::kLatency;
    entry.latency.stage = LatencyStage::kUdpRecv;
    entry.latency.cycles = static_cast<std::uint64_t>(i);
    EXPECT_TRUE(logger.md_queue().try_push(entry));
  }

  // Stop immediately — logger must final-drain.
  logger.stop();

  EXPECT_EQ(logger.entries_written(), kCount);
}

// -- Log macro tests --

TEST_F(AsyncLoggerTest, LogMacroLatency) {
  AsyncLogger logger(log_path_.c_str(), 256);
  logger.start();

  EXPECT_TRUE(log_latency(logger.md_queue(), kThreadIdMd,
                           LatencyStage::kStrategy, 200));

  usleep(50'000);
  logger.stop();

  EXPECT_EQ(logger.entries_written(), 1);
  auto content = read_file(log_path_);
  EXPECT_NE(content.find("stage=Strategy"), std::string::npos);
  EXPECT_NE(content.find("cycles=200"), std::string::npos);
}

TEST_F(AsyncLoggerTest, LogMacroOrder) {
  AsyncLogger logger(log_path_.c_str(), 256);
  logger.start();

  EXPECT_TRUE(log_order(logger.strategy_queue(), kThreadIdStrategy,
                         LogLevel::kInfo, OrderEvent::kFill, 1, 0, 10050, 10,
                         42, 99, 5));

  usleep(50'000);
  logger.stop();

  EXPECT_EQ(logger.entries_written(), 1);
  auto content = read_file(log_path_);
  EXPECT_NE(content.find("event=Fill"), std::string::npos);
  EXPECT_NE(content.find("xoid=99"), std::string::npos);
  EXPECT_NE(content.find("rem=5"), std::string::npos);
}

TEST_F(AsyncLoggerTest, LogMacroText) {
  AsyncLogger logger(log_path_.c_str(), 256);
  logger.start();

  EXPECT_TRUE(
      log_text(logger.md_queue(), kThreadIdMain, LogLevel::kWarn, "test msg"));

  usleep(50'000);
  logger.stop();

  EXPECT_EQ(logger.entries_written(), 1);
  auto content = read_file(log_path_);
  EXPECT_NE(content.find("WARN"), std::string::npos);
  EXPECT_NE(content.find("test msg"), std::string::npos);
}

TEST_F(AsyncLoggerTest, LogMacroConnection) {
  AsyncLogger logger(log_path_.c_str(), 256);
  logger.start();

  EXPECT_TRUE(log_connection(logger.strategy_queue(), kThreadIdStrategy,
                              LogLevel::kInfo, ConnectionEvent::kConnect));

  usleep(50'000);
  logger.stop();

  EXPECT_EQ(logger.entries_written(), 1);
  auto content = read_file(log_path_);
  EXPECT_NE(content.find("event=Connect"), std::string::npos);
}

TEST_F(AsyncLoggerTest, LogMacroMarketData) {
  AsyncLogger logger(log_path_.c_str(), 256);
  logger.start();

  EXPECT_TRUE(log_market_data(logger.md_queue(), kThreadIdMd, LogLevel::kInfo,
                               500, 3, 1, 30000, 25));

  usleep(50'000);
  logger.stop();

  EXPECT_EQ(logger.entries_written(), 1);
  auto content = read_file(log_path_);
  EXPECT_NE(content.find("seq=500"), std::string::npos);
  EXPECT_NE(content.find("sym=3"), std::string::npos);
}

} // namespace
} // namespace mk::sys::log
