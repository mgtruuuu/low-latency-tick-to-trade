/**
 * @file feed_handler.hpp
 * @brief UDP market data feed handler with sequence gap detection.
 *
 * Receives raw UDP datagrams, deserializes MarketDataUpdate messages,
 * and tracks sequence numbers for gap detection. This is the first
 * component in the tick-to-trade pipeline.
 *
 * HFT context:
 *   Feed handlers are the first component in any trading system. They
 *   sit at the network boundary and convert raw wire data into structured
 *   events. In production, they also handle:
 *   - A/B feed arbitration (two redundant feeds for packet loss recovery)
 *   - Snapshot + incremental reconciliation
 *   - Instrument filtering (drop irrelevant symbols early)
 *
 *   This implementation is deliberately thin — it validates and deserializes.
 *   It does NOT own the order book; it just produces MarketDataUpdate events
 *   that the strategy consumes.
 *
 * Design:
 *   - Zero allocation (all state is inline).
 *   - Single function call per datagram (on_udp_data).
 *   - Returns bool + output ref (hot-path pattern, avoids optional overhead).
 */

#pragma once

#include "shared/protocol.hpp"
#include "shared/protocol_codec.hpp"

#include "sys/log/signal_logger.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mk::app {

class FeedHandler {
public:
  /// Process a raw UDP datagram.
  /// @param buf Raw datagram bytes.
  /// @param len Number of bytes received.
  /// @param out Deserialized MarketDataUpdate (valid only when returning true).
  /// @return true on success, false on parse error or duplicate.
  [[nodiscard]] bool on_udp_data(const char *buf, std::size_t len,
                                 MarketDataUpdate &out) noexcept {
    auto view = std::as_bytes(std::span<const char>{buf, len});
    if (!deserialize_market_data(view, out)) [[unlikely]] {
      parse_errors_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    // First packet: accept whatever seq as baseline (late multicast join).
    // Without this, starting MD Publisher before tick_to_trade produces
    // a false gap warning (e.g., "expected=1 got=7025 gap=7024").
    if (!initialized_) [[unlikely]] {
      initialized_ = true;
      expected_seq_ = out.seq_num + 1;
      total_updates_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }

    // Sequence gap detection.
    // In production, a gap triggers a recovery mechanism (retransmission
    // request or switch to snapshot feed). Here we just count gaps and
    // log them for diagnostic purposes.
    if (out.seq_num != expected_seq_) [[unlikely]] {
      if (out.seq_num > expected_seq_) {
        auto gap_size = out.seq_num - expected_seq_;
        gap_count_.fetch_add(gap_size, std::memory_order_relaxed);
        sys::log::signal_log("[FEED] Gap detected: expected=", expected_seq_,
                             " got=", out.seq_num, " gap=", gap_size, '\n');
      }
      // seq < expected means duplicate or reorder — skip.
      if (out.seq_num < expected_seq_) {
        duplicate_count_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
    }

    expected_seq_ = out.seq_num + 1;
    total_updates_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // -- Observers --

  [[nodiscard]] std::uint64_t expected_seq() const noexcept {
    return expected_seq_;
  }
  [[nodiscard]] std::uint64_t gap_count() const noexcept {
    return gap_count_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t duplicate_count() const noexcept {
    return duplicate_count_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t parse_errors() const noexcept {
    return parse_errors_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t total_updates() const noexcept {
    return total_updates_.load(std::memory_order_relaxed);
  }

private:
  bool initialized_{false};       // First packet sets baseline seq.
  std::uint64_t expected_seq_{0}; // Sequencing state — not a diagnostic counter.

  // Diagnostic counters — std::atomic for cross-thread monitoring safety.
  // On x86-64, relaxed atomic store compiles to a plain MOV (zero overhead).
  // This allows a future monitoring thread to read counters without data race,
  // even though the current architecture is single-threaded.
  std::atomic<std::uint64_t> gap_count_{0};
  std::atomic<std::uint64_t> duplicate_count_{0};
  std::atomic<std::uint64_t> parse_errors_{0};
  std::atomic<std::uint64_t> total_updates_{0};
};

} // namespace mk::app
