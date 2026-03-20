/**
 * @file log_macros.hpp
 * @brief Inline push helpers for async logger — hot-path binary logging.
 *
 * Each helper constructs a LogEntry on the stack and pushes it into the
 * caller-provided SPSC queue. Cost: memcpy + atomic release store (~5-10ns).
 * All functions return bool (try_push result) — caller can optionally track
 * drops.
 *
 * HFT context:
 *   These are the hot-path logging calls. No string formatting, no allocation.
 *   Text conversion happens on the logger thread (cold path).
 */

#pragma once

#include "sys/log/async_log_entry.hpp"
#include "sys/memory/spsc_queue.hpp"
#include "sys/nano_clock.hpp"

#include <cstring>

namespace mk::sys::log {

/// Queue type alias (matches async_logger.hpp).
using LogQueue = mk::sys::memory::SPSCQueue<LogEntry>;

// =============================================================================
// Latency logging
// =============================================================================

/// Log a latency measurement (pipeline stage timing).
/// @param queue    SPSC queue for this thread.
/// @param tid      Logical thread ID (kThreadIdMd, kThreadIdStrategy, etc.).
/// @param stage    Pipeline stage being measured.
/// @param cycles   Elapsed TSC cycles for this stage.
/// @param recv_tsc UDP recv timestamp (for tick-to-trade, 0 otherwise).
/// @return true if pushed, false if queue full (dropped).
[[nodiscard]] inline bool log_latency(LogQueue &queue, std::uint16_t tid,
                                      LatencyStage stage,
                                      std::uint64_t cycles,
                                      std::uint64_t recv_tsc = 0) noexcept {
  LogEntry entry{};
  entry.tsc_timestamp = rdtsc();
  entry.thread_id = tid;
  entry.level = LogLevel::kInfo;
  entry.event_type = LogEventType::kLatency;
  entry.latency.stage = stage;
  entry.latency.cycles = cycles;
  entry.latency.recv_tsc = recv_tsc;
  return queue.try_push(entry);
}

// =============================================================================
// Order logging
// =============================================================================

/// Log an order lifecycle event (new, ack, fill, cancel, modify, etc.).
/// @return true if pushed, false if queue full (dropped).
[[nodiscard]] inline bool
log_order(LogQueue &queue, std::uint16_t tid, LogLevel level,
          OrderEvent sub_type, std::uint32_t symbol_id, std::uint8_t side,
          std::int64_t price, std::uint32_t qty,
          std::uint64_t client_order_id,
          std::uint64_t exchange_order_id = 0,
          std::uint32_t remaining_qty = 0) noexcept {
  LogEntry entry{};
  entry.tsc_timestamp = rdtsc();
  entry.thread_id = tid;
  entry.level = level;
  entry.event_type = LogEventType::kOrder;
  entry.order.sub_type = sub_type;
  entry.order.side = side;
  entry.order.symbol_id = symbol_id;
  entry.order.client_order_id = client_order_id;
  entry.order.exchange_order_id = exchange_order_id;
  entry.order.price = price;
  entry.order.qty = qty;
  entry.order.remaining_qty = remaining_qty;
  return queue.try_push(entry);
}

// =============================================================================
// Market data logging
// =============================================================================

/// Log a market data event (recv, gap, etc.).
/// @return true if pushed, false if queue full (dropped).
[[nodiscard]] inline bool log_market_data(LogQueue &queue, std::uint16_t tid,
                                          LogLevel level,
                                          std::uint64_t seq_num,
                                          std::uint32_t symbol_id,
                                          std::uint8_t side,
                                          std::int64_t price,
                                          std::uint32_t qty,
                                          std::uint32_t gap_size = 0) noexcept {
  LogEntry entry{};
  entry.tsc_timestamp = rdtsc();
  entry.thread_id = tid;
  entry.level = level;
  entry.event_type = LogEventType::kMarketData;
  entry.market_data.seq_num = seq_num;
  entry.market_data.symbol_id = symbol_id;
  entry.market_data.side = side;
  entry.market_data.price = price;
  entry.market_data.qty = qty;
  entry.market_data.gap_size = gap_size;
  return queue.try_push(entry);
}

// =============================================================================
// Connection logging
// =============================================================================

/// Log a connection lifecycle event (connect, disconnect, heartbeat, etc.).
/// @return true if pushed, false if queue full (dropped).
[[nodiscard]] inline bool log_connection(LogQueue &queue, std::uint16_t tid,
                                         LogLevel level,
                                         ConnectionEvent sub_type,
                                         std::uint32_t attempt = 0,
                                         std::int64_t rtt_ns = 0) noexcept {
  LogEntry entry{};
  entry.tsc_timestamp = rdtsc();
  entry.thread_id = tid;
  entry.level = level;
  entry.event_type = LogEventType::kConnection;
  entry.connection.sub_type = sub_type;
  entry.connection.attempt = attempt;
  entry.connection.rtt_ns = rtt_ns;
  return queue.try_push(entry);
}

// =============================================================================
// Text logging (cold-path diagnostic)
// =============================================================================

/// Log a free-form text message. Truncated at 111 characters.
/// Use sparingly — text logging is for cold-path diagnostics only.
/// @return true if pushed, false if queue full (dropped).
[[nodiscard]] inline bool log_text(LogQueue &queue, std::uint16_t tid,
                                   LogLevel level,
                                   const char *msg) noexcept {
  LogEntry entry{};
  entry.tsc_timestamp = rdtsc();
  entry.thread_id = tid;
  entry.level = level;
  entry.event_type = LogEventType::kText;

  // Copy up to 111 chars + null terminator (text.msg is 112 bytes).
  std::size_t i = 0;
  while (i < 111 && msg[i] != '\0') {
    entry.text.msg[i] = msg[i];
    ++i;
  }
  entry.text.msg[i] = '\0';
  return queue.try_push(entry);
}

} // namespace mk::sys::log
