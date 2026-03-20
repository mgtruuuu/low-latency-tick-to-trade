/**
 * @file order_send_handler_test.cpp
 * @brief Tests for OrderSendHandler — signal→serialize→send dispatch,
 *        risk rejection pass-through, modify-vs-new path selection.
 *
 * Uses socketpair() to create a connected TCP-like fd pair for send testing
 * without requiring a real network connection.
 */

#include "tick_to_trade/order_send_handler.hpp"

#include "tick_to_trade/latency_tracker.hpp"
#include "tick_to_trade/order_manager.hpp"
#include "tick_to_trade/spread_strategy.hpp"
#include "tick_to_trade/strategy_ctx.hpp"
#include "tick_to_trade/tcp_connection.hpp"

#include "shared/protocol.hpp"
#include "shared/protocol_codec.hpp"

#include "net/message_codec.hpp"
#include "net/tcp_socket.hpp"

#include "sys/log/async_log_entry.hpp"
#include "sys/log/log_macros.hpp"
#include "sys/memory/spsc_queue.hpp"
#include "sys/nano_clock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <span>
#include <sys/socket.h>
#include <unistd.h>

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
// Fixture
// ---------------------------------------------------------------------------

class OrderSendHandlerTest : public ::testing::Test {
protected:
  void SetUp() override {
    ctx_ = make_strategy_ctx<TestStrategy>(ctx_buf_.data(), 0, 0, 0,
                                           kMaxOutstanding, kMaxOutstanding,
                                           kWheelSize, kMaxTimers);
    om_ = std::construct_at(
        reinterpret_cast<OrderManager *>(om_storage_),
        ctx_,
        /*max_position=*/1000,
        /*max_order_size=*/100,
        /*max_notional=*/10'000'000,
        /*max_orders_per_window=*/100,
        /*rate_window_ns=*/1'000'000'000,
        /*order_timeout_ns=*/kTimeoutNs);

    // Create a connected socket pair for send testing.
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    send_sock_ = std::make_unique<net::TcpSocket>(fds[0]);
    recv_fd_ = fds[1];
  }

  void TearDown() override {
    std::destroy_at(om_);
    send_sock_.reset();
    if (recv_fd_ >= 0) {
      ::close(recv_fd_);
    }
  }

  // Read data sent through the socket pair and verify it's a valid TLV frame.
  bool read_and_verify_frame(MsgType expected_type) const {
    std::array<char, 512> buf{};
    auto n = ::recv(recv_fd_, buf.data(), buf.size(), 0);
    if (n <= 0) {
      return false;
    }

    net::ParsedMessageView msg;
    if (!net::unpack_message(
            std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(buf.data()),
                static_cast<std::size_t>(n)),
            msg)) {
      return false;
    }
    return static_cast<MsgType>(msg.header.msg_type) == expected_type;
  }

  // Dispatch a signal through the send handler.
  bool dispatch_signal(const Signal &signal) {
    return handler_.on_signal(
        signal, *om_, tracker_, *send_sock_, scratch_, tcp_tx_, sys::rdtsc(),
        conn_,
        *log_queue_); // NOLINT(bugprone-unchecked-optional-access)
  }

  // -- Members --
  std::array<std::byte,
             strategy_ctx_buf_size<TestStrategy>(0, 0, 0, kMaxOutstanding,
                                                 kWheelSize, kMaxTimers)>
      ctx_buf_{};
  StrategyCtx ctx_{};
  alignas(OrderManager) std::byte om_storage_[sizeof(OrderManager)]{};
  OrderManager *om_{nullptr};

  OrderSendHandler handler_;
  LatencyTracker tracker_;
  ConnectionState conn_;

  std::unique_ptr<net::TcpSocket> send_sock_;
  int recv_fd_{-1};

  // Scratch and TX buffers.
  std::array<std::byte, 256> scratch_{};
  std::array<std::byte, 256> tcp_tx_{};

  // LogQueue.
  std::array<sys::log::LogEntry, 16> log_buf_{};
  std::optional<sys::log::LogQueue> log_queue_{
      sys::log::LogQueue::create(log_buf_.data(), 16)};
};

// ---------------------------------------------------------------------------
// NewOrder path
// ---------------------------------------------------------------------------

TEST_F(OrderSendHandlerTest, NewOrderSerializesAndSends) {
  const Signal sig{.side = algo::Side::kBid,
                   .price = 10000,
                   .qty = 10,
                   .symbol_id = 1};

  EXPECT_TRUE(dispatch_signal(sig));
  EXPECT_EQ(handler_.orders_serialized(), 1U);
  EXPECT_EQ(om_->outstanding_count(), 1U);
  EXPECT_TRUE(read_and_verify_frame(MsgType::kNewOrder));
}

TEST_F(OrderSendHandlerTest, RiskRejectReturnsTrue) {
  // Fill up to max outstanding.
  for (std::uint32_t i = 0; i < kMaxOutstanding; ++i) {
    const Signal sig{.side = algo::Side::kBid,
                     .price = 10000,
                     .qty = 10,
                     .symbol_id = 1};
    ASSERT_TRUE(dispatch_signal(sig));
  }

  // Next signal should be risk-rejected — returns true (no connection death).
  const Signal sig{.side = algo::Side::kBid,
                   .price = 10000,
                   .qty = 10,
                   .symbol_id = 1};
  EXPECT_TRUE(dispatch_signal(sig));
  EXPECT_EQ(handler_.orders_serialized(), kMaxOutstanding);
}

// ---------------------------------------------------------------------------
// Modify path
// ---------------------------------------------------------------------------

TEST_F(OrderSendHandlerTest, ModifyPathTakenForRestingOrder) {
  // Send an order.
  const Signal sig{.side = algo::Side::kBid,
                   .price = 10000,
                   .qty = 10,
                   .symbol_id = 1};
  ASSERT_TRUE(dispatch_signal(sig));
  // Drain the socket for the NewOrder frame.
  {
    std::array<char, 512> drain{};
    (void)::recv(recv_fd_, drain.data(), drain.size(), 0);
  }

  // Ack the order to make it resting.
  OrderAck ack{};
  ack.client_order_id = 1;
  ack.exchange_order_id = 1000;
  ack.send_ts = 0;
  om_->on_order_ack(ack);

  // Same symbol, different price — should trigger modify path.
  const Signal modify_sig{.side = algo::Side::kBid,
                          .price = 11000,
                          .qty = 10,
                          .symbol_id = 1};
  EXPECT_TRUE(dispatch_signal(modify_sig));
  EXPECT_EQ(handler_.modifies_serialized(), 1U);
  // orders_serialized should NOT increment for modify.
  EXPECT_EQ(handler_.orders_serialized(), 1U);
  EXPECT_TRUE(read_and_verify_frame(MsgType::kModifyOrder));
}

// ---------------------------------------------------------------------------
// Connection failure
// ---------------------------------------------------------------------------

TEST_F(OrderSendHandlerTest, ClosedSocketReturnsFalse) {
  // Close the recv end to simulate peer disconnect.
  ::close(recv_fd_);
  recv_fd_ = -1;

  Signal sig{.side = algo::Side::kBid,
             .price = 10000,
             .qty = 10,
             .symbol_id = 1};
  // Send may succeed initially (buffered) or fail depending on OS.
  // Send enough to trigger EPIPE.
  bool send_ok = true;
  for (int i = 0; i < 100 && send_ok; ++i) {
    sig.price = 10000 + i; // Vary price to avoid modify path.
    // Reset OrderManager by filling each order.
    if (om_->outstanding_count() >= kMaxOutstanding) {
      for (std::uint32_t j = 1; j <= kMaxOutstanding; ++j) {
        FillReport fill{};
        fill.client_order_id = om_->orders_sent() - kMaxOutstanding + j;
        fill.exchange_order_id = 1000;
        fill.fill_price = 10000;
        fill.fill_qty = 10;
        fill.remaining_qty = 0;
        fill.send_ts = 0;
        om_->on_fill(fill);
      }
    }
    send_ok = dispatch_signal(sig);
  }
  // At some point, send must fail.
  EXPECT_FALSE(send_ok);
  EXPECT_GE(conn_.send_failures.load(), 1U);
}

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

TEST_F(OrderSendHandlerTest, OrdersSerializedCountsCorrectly) {
  for (int i = 0; i < 3; ++i) {
    const Signal sig{.side = (i % 2 == 0) ? algo::Side::kBid : algo::Side::kAsk,
                     .price = 10000,
                     .qty = 10,
                     .symbol_id = 1};
    ASSERT_TRUE(dispatch_signal(sig));
    // Drain socket.
    std::array<char, 512> drain{};
    (void)::recv(recv_fd_, drain.data(), drain.size(), 0);
    // Fill to free outstanding slot.
    FillReport fill{};
    fill.client_order_id = static_cast<std::uint64_t>(i) + 1;
    fill.exchange_order_id = 1000;
    fill.fill_price = 10000;
    fill.fill_qty = 10;
    fill.remaining_qty = 0;
    fill.send_ts = 0;
    om_->on_fill(fill);
  }
  EXPECT_EQ(handler_.orders_serialized(), 3U);
  EXPECT_EQ(handler_.modifies_serialized(), 0U);
}

} // namespace
} // namespace mk::app
