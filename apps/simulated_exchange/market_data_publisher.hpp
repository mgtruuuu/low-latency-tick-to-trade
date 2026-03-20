/**
 * @file market_data_publisher.hpp
 * @brief Synthetic market data generator and UDP multicast publisher.
 *
 * Generates realistic-enough market data by simulating a random walk
 * around a reference price with mean reversion. Publishes individual
 * bid/ask updates via UDP multicast, each carrying a monotonically
 * increasing sequence number for gap detection.
 *
 * HFT context:
 *   Exchanges publish market data via UDP multicast. Each packet carries
 *   a sequence number. Gaps indicate packet loss and trigger recovery
 *   (retransmission request or feed switch). This publisher simulates
 *   that behavior for end-to-end pipeline testing.
 *
 * A/B Feed Redundancy:
 *   Real exchanges publish identical data on two independent multicast
 *   groups (Feed A and Feed B) over different network paths. If one path
 *   drops a packet, the other likely still has it. Clients subscribe to
 *   both, deduplicate by seq_num, and use whichever arrives first.
 *   This publisher supports optional Feed B via a second destination.
 *
 * Multi-symbol:
 *   Multiple publishers can share a single seq_num counter to produce
 *   a unified stream (like Nasdaq ITCH). Each publisher stamps its own
 *   symbol_id on every message. The client filters by symbol_id.
 *
 * Design:
 *   - Pre-allocated send buffer (no allocation on hot path).
 *   - xorshift64 PRNG (no std::mt19937 — too heavy for hot path).
 *   - Publishes both bid and ask on each tick (2 UDP datagrams).
 */

#pragma once

#include "algo/trading_types.hpp"
#include "shared/protocol.hpp"
#include "shared/protocol_codec.hpp"

#include "net/udp_socket.hpp"
#include "net/udp_socket_concept.hpp"
#include "sys/log/signal_logger.hpp"
#include "sys/nano_clock.hpp"
#include "sys/xorshift64.hpp"

#include <arpa/inet.h>
#include <array>
#include <cstddef>
#include <cstdint>

namespace mk::app {

template <net::UdpSendable UdpSock = net::UdpSocket> class MarketDataPublisher {
public:
  /// @param sock UDP socket for sending (shared across publishers).
  /// @param symbol_id Instrument identifier stamped on every message.
  /// @param shared_seq [in/out] Shared sequence number counter (for
  ///        multi-symbol unified stream). Incremented on every publish.
  /// @param feed_a_dest Feed A multicast destination.
  /// @param feed_b_dest Optional Feed B destination (nullptr = no B feed).
  MarketDataPublisher(UdpSock &sock, std::uint32_t symbol_id,
                      std::uint64_t &shared_seq, const sockaddr_in &feed_a_dest,
                      const sockaddr_in *feed_b_dest = nullptr) noexcept
      : sock_{sock}, symbol_id_{symbol_id}, shared_seq_{shared_seq},
        feed_a_dest_{feed_a_dest}, feed_b_dest_{feed_b_dest} {
  }

  /// Publish a pair of bid/ask market data updates (2 UDP datagrams).
  /// Called periodically from the exchange's main loop.
  void publish_tick() noexcept {
    const auto [bid_price, bid_qty, ask_price, ask_qty] = generate_quotes();
    const std::int64_t now = sys::monotonic_nanos();

    publish_update(algo::Side::kBid, bid_price, bid_qty, now);
    publish_update(algo::Side::kAsk, ask_price, ask_qty, now);
  }

  /// Publish a trade event (after a fill occurs in the matching engine).
  /// Updates the mid-price to reflect the trade.
  void publish_trade(algo::Side aggressor_side, algo::Price trade_price,
                     algo::Qty trade_qty) noexcept {
    // Shift mid-price toward trade price (market impact).
    mid_price_ = (mid_price_ + trade_price) / 2;

    const std::int64_t now = sys::monotonic_nanos();

    // Publish updated bid/ask reflecting the trade.
    const algo::Price bid_price = mid_price_ - kHalfSpread;
    const algo::Price ask_price = mid_price_ + kHalfSpread;

    publish_update(aggressor_side, trade_price, trade_qty, now);

    // Also publish updated opposite side.
    const auto other_side = (aggressor_side == algo::Side::kBid)
                                ? algo::Side::kAsk
                                : algo::Side::kBid;
    const algo::Price other_price =
        (other_side == algo::Side::kBid) ? bid_price : ask_price;
    const auto other_qty = static_cast<algo::Qty>(100 + (rng_() % 901));
    publish_update(other_side, other_price, other_qty, now);
  }

  /// Publish a single market data update (1 UDP datagram).
  /// Public so OrderGateway can publish real BBO from the OrderBook.
  void publish_update(algo::Side side, algo::Price price,
                      algo::Qty qty) noexcept {
    publish_update(side, price, qty, sys::monotonic_nanos());
  }

  [[nodiscard]] std::uint64_t seq_num() const noexcept { return shared_seq_; }
  [[nodiscard]] algo::Price mid_price() const noexcept { return mid_price_; }
  [[nodiscard]] std::uint32_t symbol_id() const noexcept { return symbol_id_; }

private:
  // Tick size: 1 cent in fixed-point (price * 10000).
  // Reference price: $100.0000.
  // Half-spread: 5 ticks ($0.0005).
  static constexpr algo::Price kTickSize = 100;              // $0.01
  static constexpr algo::Price kHalfSpread = 500;            // $0.05
  static constexpr algo::Price kDefaultRefPrice = 1'000'000; // $100.00

  UdpSock &sock_;
  std::uint32_t symbol_id_;
  std::uint64_t
      &shared_seq_;
  sockaddr_in feed_a_dest_;
  const sockaddr_in *feed_b_dest_; // nullptr = no Feed B
  algo::Price ref_price_{kDefaultRefPrice};
  algo::Price mid_price_{kDefaultRefPrice};

  // Pre-allocated send buffer — fits one MarketDataUpdate datagram.
  std::array<std::byte, 128> send_buf_{};

  // Debug: fixed seed for reproducible sequences during development.
  // Release: time-based seed so each run generates different market data.
#ifdef NDEBUG
  sys::Xorshift64 rng_{static_cast<std::uint64_t>(sys::monotonic_nanos())};
#else
  sys::Xorshift64 rng_{0x12345678'9ABCDEF0ULL};
#endif

  struct QuoteSnapshot {
    algo::Price bid_price;
    algo::Qty bid_qty;
    algo::Price ask_price;
    algo::Qty ask_qty;
  };

  /// Advance mid-price via random walk + mean reversion, then derive quotes.
  QuoteSnapshot generate_quotes() noexcept {
    // Random walk: move price by -3 to +3 ticks.
    const auto jitter = static_cast<std::int64_t>(rng_() % 7) - 3;
    mid_price_ += jitter * kTickSize;

    // Mean reversion: pull 10% toward reference price each tick.
    const algo::Price drift = (ref_price_ - mid_price_) / 10;
    mid_price_ += drift;

    return {.bid_price = mid_price_ - kHalfSpread,
            .bid_qty = static_cast<algo::Qty>(100 + (rng_() % 901)),
            .ask_price = mid_price_ + kHalfSpread,
            .ask_qty = static_cast<algo::Qty>(100 + (rng_() % 901))};
  }

  void publish_update(algo::Side side, algo::Price price, algo::Qty qty,
                      std::int64_t ts) noexcept {
    const MarketDataUpdate md{
        .seq_num = shared_seq_++,
        .symbol_id = symbol_id_,
        .side = side,
        .price = price,
        .qty = qty,
        .exchange_ts = ts,
    };

    const auto bytes = serialize_market_data(send_buf_, md);
    if (bytes == 0) [[unlikely]] {
      return;
    }

    // Send to Feed A.
    const auto result = sock_.sendto_nonblocking(
        reinterpret_cast<const char *>(send_buf_.data()), bytes, feed_a_dest_);
    if (result.status != UdpSock::SendtoStatus::kOk) [[unlikely]] {
      sys::log::signal_log(
          "[EXCHANGE] UDP send (A) failed: errno=", result.err_no, '\n');
    }

    // Send identical data to Feed B (A/B redundancy).
    if (feed_b_dest_) {
      const auto result_b = sock_.sendto_nonblocking(
          reinterpret_cast<const char *>(send_buf_.data()), bytes,
          *feed_b_dest_);
      if (result_b.status != UdpSock::SendtoStatus::kOk) [[unlikely]] {
        sys::log::signal_log(
            "[EXCHANGE] UDP send (B) failed: errno=", result_b.err_no, '\n');
      }
    }
  }
};

} // namespace mk::app
