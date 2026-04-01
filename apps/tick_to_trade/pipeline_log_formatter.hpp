/**
 * @file pipeline_log_formatter.hpp
 * @brief Text formatter for tick-to-trade pipeline LogEntry.
 *
 * Stateless functor that converts a LogEntry into a human-readable text line.
 * Used as the FormatterT parameter for AsyncDrainLoop<LogEntry, ...>.
 * All string tables and formatting helpers are defined here.
 */

#pragma once

#include "pipeline_log_entry.hpp"

#include "sys/nano_clock.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace mk::app {

namespace detail {

// Compile-time check: all entries in a table must have equal width.
template <std::size_t N>
constexpr bool all_same_width(const std::string_view (&arr)[N]) {
  for (std::size_t i = 1; i < N; ++i) {
    if (arr[i].size() != arr[0].size()) {
      return false;
    }
  }
  return true;
}

using namespace std::string_view_literals;

// NOLINTBEGIN(*-avoid-c-arrays)
constexpr std::string_view kThreadNames[] = {"MAIN "sv, "MD   "sv, "STRAT"sv,
                                             "LOG  "sv};
constexpr std::string_view kLevelNames[] = {"DEBUG"sv, "INFO "sv, "WARN "sv,
                                            "ERROR"sv};
constexpr std::string_view kEventTypeNames[] = {
    "LATENCY"sv, "ORDER  "sv, "MKTDATA"sv, "CONN   "sv, "TEXT   "sv};
static_assert(all_same_width(kThreadNames), "kThreadNames: width mismatch");
static_assert(all_same_width(kLevelNames), "kLevelNames: width mismatch");
static_assert(all_same_width(kEventTypeNames),
              "kEventTypeNames: width mismatch");
constexpr std::string_view kLatencyStageNames[] = {"UdpRecv"sv,   "FeedParse"sv,
                                                   "QueueHop"sv,  "Strategy"sv,
                                                   "OrderSend"sv, "T2T"sv};
constexpr std::string_view kOrderEventNames[] = {
    "NewOrder"sv,   "OrderAck"sv,    "OrderReject"sv,  "Fill"sv,
    "CancelSent"sv, "CancelAck"sv,   "CancelReject"sv, "ModifySent"sv,
    "ModifyAck"sv,  "ModifyReject"sv};
constexpr std::string_view kConnectionEventNames[] = {
    "Connect"sv, "Disconnect"sv, "HbSent"sv, "HbRecv"sv, "Reconnect"sv};
constexpr std::string_view kSideNames[] = {"Bid"sv, "Ask"sv};
// NOLINTEND(*-avoid-c-arrays)

inline std::size_t append_str(char *buf, std::size_t pos, std::size_t cap,
                              std::string_view sv) noexcept {
  const auto max_len = cap - 1 - pos;
  const auto len = std::min(sv.size(), max_len);
  std::memcpy(buf + pos, sv.data(), len);
  return pos + len;
}

inline std::size_t append_u64(char *buf, std::size_t pos, std::size_t cap,
                              std::uint64_t val) noexcept {
  if (pos >= cap - 1) {
    return pos;
  }
  auto result = std::to_chars(buf + pos, buf + cap - 1, val);
  return static_cast<std::size_t>(result.ptr - buf);
}

inline std::size_t append_i64(char *buf, std::size_t pos, std::size_t cap,
                              std::int64_t val) noexcept {
  if (pos >= cap - 1) {
    return pos;
  }
  auto result = std::to_chars(buf + pos, buf + cap - 1, val);
  return static_cast<std::size_t>(result.ptr - buf);
}

inline std::size_t append_u32(char *buf, std::size_t pos, std::size_t cap,
                              std::uint32_t val) noexcept {
  if (pos >= cap - 1) {
    return pos;
  }
  auto result = std::to_chars(buf + pos, buf + cap - 1, val);
  return static_cast<std::size_t>(result.ptr - buf);
}

} // namespace detail

/// Stateless formatter for pipeline LogEntry → text line.
/// Satisfies EntryFormatter<PipelineLogFormatter, LogEntry>.
struct PipelineLogFormatter {
  std::size_t operator()(const LogEntry &entry,
                         const mk::sys::TscCalibration &tsc_cal,
                         std::span<char> buf) const noexcept {
    using namespace detail;
    using namespace std::string_view_literals;

    std::size_t pos = 0;
    const auto cap = buf.size();
    auto *const data = buf.data();

    // TSC timestamp → nanoseconds.
    const auto ns =
        static_cast<std::uint64_t>(tsc_cal.to_ns(entry.tsc_timestamp));
    pos = append_u64(data, pos, cap, ns);
    data[pos++] = ' ';

    // Thread name.
    const auto tid = std::min(entry.thread_id, static_cast<std::uint16_t>(3));
    pos = append_str(data, pos, cap, kThreadNames[tid]);
    data[pos++] = ' ';

    // Level.
    pos = append_str(data, pos, cap,
                     kLevelNames[static_cast<std::uint8_t>(entry.level)]);
    data[pos++] = ' ';

    // Event type.
    pos = append_str(
        data, pos, cap,
        kEventTypeNames[static_cast<std::uint8_t>(entry.event_type)]);
    data[pos++] = ' ';
    data[pos++] = ' ';

    // Event-specific fields.
    switch (entry.event_type) {
    case LogEventType::kLatency: {
      pos = append_str(data, pos, cap, "stage="sv);
      pos = append_str(
          data, pos, cap,
          kLatencyStageNames[static_cast<std::uint8_t>(entry.latency.stage)]);
      pos = append_str(data, pos, cap, " cycles="sv);
      pos = append_u64(data, pos, cap, entry.latency.cycles);
      pos = append_str(data, pos, cap, " ns="sv);
      const auto lat_ns =
          static_cast<std::uint64_t>(tsc_cal.to_ns(entry.latency.cycles));
      pos = append_u64(data, pos, cap, lat_ns);
      break;
    }
    case LogEventType::kOrder: {
      pos = append_str(data, pos, cap, "event="sv);
      pos = append_str(
          data, pos, cap,
          kOrderEventNames[static_cast<std::uint8_t>(entry.order.sub_type)]);
      pos = append_str(data, pos, cap, " sym="sv);
      pos = append_u32(data, pos, cap, entry.order.symbol_id);
      pos = append_str(data, pos, cap, " side="sv);
      pos = append_str(data, pos, cap,
                       entry.order.side < 2 ? kSideNames[entry.order.side]
                                            : "?"sv);
      pos = append_str(data, pos, cap, " price="sv);
      pos = append_i64(data, pos, cap, entry.order.price);
      pos = append_str(data, pos, cap, " qty="sv);
      pos = append_u32(data, pos, cap, entry.order.qty);
      pos = append_str(data, pos, cap, " oid="sv);
      pos = append_u64(data, pos, cap, entry.order.client_order_id);
      if (entry.order.exchange_order_id != 0) {
        pos = append_str(data, pos, cap, " xoid="sv);
        pos = append_u64(data, pos, cap, entry.order.exchange_order_id);
      }
      if (entry.order.remaining_qty != 0) {
        pos = append_str(data, pos, cap, " rem="sv);
        pos = append_u32(data, pos, cap, entry.order.remaining_qty);
      }
      break;
    }
    case LogEventType::kMarketData: {
      pos = append_str(data, pos, cap, "seq="sv);
      pos = append_u64(data, pos, cap, entry.market_data.seq_num);
      pos = append_str(data, pos, cap, " sym="sv);
      pos = append_u32(data, pos, cap, entry.market_data.symbol_id);
      pos = append_str(data, pos, cap, " side="sv);
      pos = append_str(data, pos, cap,
                       entry.market_data.side < 2
                           ? kSideNames[entry.market_data.side]
                           : "?"sv);
      pos = append_str(data, pos, cap, " price="sv);
      pos = append_i64(data, pos, cap, entry.market_data.price);
      pos = append_str(data, pos, cap, " qty="sv);
      pos = append_u32(data, pos, cap, entry.market_data.qty);
      if (entry.market_data.gap_size > 0) {
        pos = append_str(data, pos, cap, " gap="sv);
        pos = append_u32(data, pos, cap, entry.market_data.gap_size);
      }
      break;
    }
    case LogEventType::kConnection: {
      pos = append_str(data, pos, cap, "event="sv);
      pos = append_str(data, pos, cap,
                       kConnectionEventNames[static_cast<std::uint8_t>(
                           entry.connection.sub_type)]);
      if (entry.connection.attempt > 0) {
        pos = append_str(data, pos, cap, " attempt="sv);
        pos = append_u32(data, pos, cap, entry.connection.attempt);
      }
      if (entry.connection.rtt_ns > 0) {
        pos = append_str(data, pos, cap, " rtt_ns="sv);
        pos = append_i64(data, pos, cap, entry.connection.rtt_ns);
      }
      break;
    }
    case LogEventType::kText: {
      pos = append_str(data, pos, cap, R"(msg=")"sv);
      const auto msg_len = ::strnlen(entry.text.msg, 111);
      pos =
          append_str(data, pos, cap, std::string_view(entry.text.msg, msg_len));
      if (pos < cap - 1) {
        data[pos++] = '"';
      }
      break;
    }
    }

    // Newline.
    if (pos < cap - 1) {
      data[pos++] = '\n';
    }

    return pos;
  }
};

} // namespace mk::app
