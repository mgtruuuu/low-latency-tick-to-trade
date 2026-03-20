/**
 * @file async_log_entry.hpp
 * @brief Fixed-size binary log entry for the async logger.
 *
 * LogEntry is a 128-byte tagged union designed for zero-allocation hot-path
 * logging via SPSCQueue. Binary on the hot path — text formatting is deferred
 * to the logger thread (cold path).
 *
 * HFT context:
 *   Industry standard: hot threads push binary structs into SPSC queues.
 *   A dedicated logger thread drains and converts to text/binary log files.
 *   This avoids string formatting overhead (~100-500ns) on the critical path.
 */

#pragma once

#include <cstdint>
#include <type_traits>

namespace mk::sys::log {

/// Log severity level.
enum class LogLevel : std::uint8_t {
  kDebug = 0,
  kInfo = 1,
  kWarn = 2,
  kError = 3,
};

/// Log event type — discriminator for the payload union.
enum class LogEventType : std::uint8_t {
  kLatency = 0,    // Pipeline stage timing measurement
  kOrder = 1,      // Order lifecycle event (new, ack, fill, cancel, ...)
  kMarketData = 2, // Market data event (recv, gap, duplicate)
  kConnection = 3, // TCP connection lifecycle
  kText = 4,       // Generic text message (cold-path diagnostics)
};

/// Pipeline stage identifier for kLatency entries.
enum class LatencyStage : std::uint8_t {
  kUdpRecv = 0,
  kFeedParse = 1,
  kQueueHop = 2,
  kStrategy = 3,
  kOrderSend = 4,
  kTickToTrade = 5,
};

/// Order event sub-type for kOrder entries.
enum class OrderEvent : std::uint8_t {
  kNewOrder = 0,
  kOrderAck = 1,
  kOrderReject = 2,
  kFill = 3,
  kCancelSent = 4,
  kCancelAck = 5,
  kCancelReject = 6,
  kModifySent = 7,
  kModifyAck = 8,
  kModifyReject = 9,
};

/// Connection event sub-type for kConnection entries.
enum class ConnectionEvent : std::uint8_t {
  kConnect = 0,
  kDisconnect = 1,
  kHeartbeatSent = 2,
  kHeartbeatRecv = 3,
  kReconnectAttempt = 4,
};

/// Fixed-size log entry — exactly 128 bytes (2 cache lines).
///
/// Why 128 bytes:
///   64 bytes is too tight for order events (symbol_id, price, qty, order_id
///   alone need ~40 bytes plus the 16-byte header). 128 bytes allows generous
///   text messages (112 chars) and fits neatly in SPSC ring buffer slots.
///   At 4096 queue capacity: 4096 * 128 = 512KB per queue — fits in L2 cache.
struct LogEntry {
  // -- Header (16 bytes) --
  std::uint64_t tsc_timestamp{0};  // rdtsc() at log site
  std::uint16_t thread_id{0};     // Logical thread ID (see kThreadId* below)
  LogLevel level{LogLevel::kInfo};
  LogEventType event_type{LogEventType::kText}; // Union discriminator
  std::uint8_t pad_[4]{};         // Align payload to 16-byte boundary

  // -- Payload (112 bytes) — tagged union --
  // C++ rule: a union may have at most ONE member with a default member
  // initializer. `latency{}` is that member — its `{}` zero-initializes
  // the entire 112-byte union storage, so all other members start zeroed
  // regardless of which event_type is active.
  union {
    // kLatency: pipeline stage timing
    struct {
      LatencyStage stage;
      std::uint8_t reserved[7];
      std::uint64_t cycles;   // Elapsed TSC cycles for this stage
      std::uint64_t recv_tsc; // UDP recv timestamp (for tick-to-trade)
    } latency{}; // 24 bytes used — default init zeroes entire union

    // kOrder: order lifecycle event
    struct {
      OrderEvent sub_type;
      std::uint8_t side;           // 0=Bid, 1=Ask
      std::uint8_t reserved[2];
      std::uint32_t symbol_id;
      std::uint64_t client_order_id;
      std::uint64_t exchange_order_id;
      std::int64_t price;
      std::uint32_t qty;
      std::uint32_t remaining_qty;
      std::int64_t send_ts;        // monotonic_nanos at order send
    } order; // 48 bytes used

    // kMarketData: market data event
    struct {
      std::uint64_t seq_num;
      std::uint32_t symbol_id;
      std::uint8_t side;           // 0=Bid, 1=Ask
      std::uint8_t reserved[3];
      std::int64_t price;
      std::uint32_t qty;
      std::uint32_t gap_size;      // Non-zero if gap detected
    } market_data; // 32 bytes used

    // kConnection: TCP connection lifecycle
    struct {
      ConnectionEvent sub_type;
      std::uint8_t reserved[3];
      std::uint32_t attempt;       // Reconnect attempt number
      std::int64_t rtt_ns;         // Heartbeat RTT in nanoseconds
    } connection; // 16 bytes used

    // kText: generic text message (cold-path diagnostic)
    struct {
      char msg[112]; // Null-terminated, truncated at 111 chars
    } text; // 112 bytes used
  };
};

static_assert(sizeof(LogEntry) == 128, "LogEntry must be exactly 128 bytes");
static_assert(std::is_trivially_copyable_v<LogEntry>,
              "LogEntry must be trivially copyable for SPSCQueue");
static_assert(std::is_trivially_destructible_v<LogEntry>,
              "LogEntry must be trivially destructible for SPSCQueue");

// Logical thread IDs for the header.
inline constexpr std::uint16_t kThreadIdMain = 0;
inline constexpr std::uint16_t kThreadIdMd = 1;
inline constexpr std::uint16_t kThreadIdStrategy = 2;
inline constexpr std::uint16_t kThreadIdLogger = 3;

} // namespace mk::sys::log
