/**
 * @file async_logger.cpp
 * @brief Async logger implementation — thread loop, text formatting, file I/O.
 */

#include "sys/log/async_logger.hpp"
#include "sys/log/signal_logger.hpp"
#include "sys/memory/mmap_utils.hpp"
#include "sys/thread/affinity.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <string_view>
#include <unistd.h>

namespace mk::sys::log {

namespace {

// -- String tables for enum-to-text formatting --

// Compile-time check: all entries in a table must have equal width (for
// column-aligned log output). Zero cost — evaluated entirely at compile time.
template <std::size_t N>
constexpr bool all_same_width(const std::string_view (&arr)[N]) {
  for (std::size_t i = 1; i < N; ++i) {
    if (arr[i].size() != arr[0].size()) {
      return false;
    }
  }
  return true;
}

// string_view tables: length is computed at compile time (no runtime strlen).
using namespace std::string_view_literals;
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
constexpr std::string_view kLatencyStageNames[] = {
    "UdpRecv"sv, "FeedParse"sv, "QueueHop"sv,
    "Strategy"sv, "OrderSend"sv, "T2T"sv};
constexpr std::string_view kOrderEventNames[] = {
    "NewOrder"sv,  "OrderAck"sv,    "OrderReject"sv, "Fill"sv,
    "CancelSent"sv, "CancelAck"sv,  "CancelReject"sv, "ModifySent"sv,
    "ModifyAck"sv,  "ModifyReject"sv};
constexpr std::string_view kConnectionEventNames[] = {
    "Connect"sv, "Disconnect"sv, "HbSent"sv, "HbRecv"sv, "Reconnect"sv};
constexpr std::string_view kSideNames[] = {"Bid"sv, "Ask"sv};

// Append a string_view to buffer via memcpy. Returns new position.
// For string literals, .size() is a compile-time constant — the compiler can
// inline the memcpy as 1-2 MOV instructions (same effect as __builtin_memcpy).
std::size_t append_str(char *buf, std::size_t pos, std::size_t cap,
                       std::string_view sv) noexcept {
  const auto max_len = cap - 1 - pos;
  const auto len = std::min(sv.size(), max_len);
  std::memcpy(buf + pos, sv.data(), len);
  return pos + len;
}

// Append a uint64 in decimal. Returns new position.
std::size_t append_u64(char *buf, std::size_t pos, std::size_t cap,
                       std::uint64_t val) noexcept {
  if (pos >= cap - 1) {
    return pos;
  }
  auto result =
      std::to_chars(buf + pos, buf + cap - 1, val);
  return static_cast<std::size_t>(result.ptr - buf);
}

// Append a signed int64 in decimal. Returns new position.
std::size_t append_i64(char *buf, std::size_t pos, std::size_t cap,
                       std::int64_t val) noexcept {
  if (pos >= cap - 1) {
    return pos;
  }
  auto result =
      std::to_chars(buf + pos, buf + cap - 1, val);
  return static_cast<std::size_t>(result.ptr - buf);
}

// Append a uint32 in decimal.
std::size_t append_u32(char *buf, std::size_t pos, std::size_t cap,
                       std::uint32_t val) noexcept {
  if (pos >= cap - 1) {
    return pos;
  }
  auto result =
      std::to_chars(buf + pos, buf + cap - 1, val);
  return static_cast<std::size_t>(result.ptr - buf);
}

[[nodiscard]] memory::RegionIntentConfig
build_hot_queue_alloc_cfg(std::uint32_t capacity, int numa_node) noexcept {
  return memory::RegionIntentConfig{
      .size = LogQueue::required_buffer_size(capacity),
      .numa_node = numa_node,
  };
}

} // namespace

// =============================================================================
// Constructor / Destructor
// =============================================================================

AsyncLogger::AsyncLogger(const char *log_path, std::uint32_t capacity,
                         int numa_node) noexcept
    : md_queue_mem_(
          memory::allocate_hot_rw_region(build_hot_queue_alloc_cfg(capacity,
                                                                   numa_node))),
      strategy_queue_mem_(
          memory::allocate_hot_rw_region(build_hot_queue_alloc_cfg(capacity,
                                                                   numa_node))),
      md_queue_(static_cast<LogEntry *>(md_queue_mem_.get()), capacity),
      strategy_queue_(static_cast<LogEntry *>(strategy_queue_mem_.get()),
                      capacity),
      log_fd_(::open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                     0644)),
      tsc_cal_(TscCalibration::calibrate()) {
  // Register producer queues for N-way merge in drain_all().
  // To add a new producer: add a LogQueue member and register here.
  queues_[0] = &md_queue_;
  queues_[1] = &strategy_queue_;

  // Derive num_queues_ from registered (non-null) entries — no magic number.
  // Adding a queue above automatically updates the count.
  for (num_queues_ = 0;
       num_queues_ < kMaxQueues && queues_[num_queues_] != nullptr;
       ++num_queues_) {
  }

  if (log_fd_ < 0) {
    signal_log("[ASYNC_LOGGER] Failed to open log file: ", log_path,
               " errno=", errno, '\n');
    std::abort();
  }
}

AsyncLogger::~AsyncLogger() noexcept {
  stop();
  if (log_fd_ >= 0) {
    ::close(log_fd_);
    log_fd_ = -1;
  }
}

// =============================================================================
// Thread Lifecycle
// =============================================================================

void AsyncLogger::start(std::int32_t pin_core) noexcept {
  thread_ = std::thread(&AsyncLogger::run, this, pin_core);
}

void AsyncLogger::stop() noexcept {
  stop_.store(true, std::memory_order_relaxed);
  if (thread_.joinable()) {
    thread_.join();
  }
}

// =============================================================================
// Logger Thread Main Loop
// =============================================================================

void AsyncLogger::run(std::int32_t pin_core) noexcept {
  if (pin_core >= 0) {
    auto result =
        mk::sys::thread::pin_current_thread(static_cast<std::uint32_t>(pin_core));
    if (result == 0) {
      signal_log("[ASYNC_LOGGER] Pinned to core ", pin_core, '\n');
    } else {
      signal_log("[ASYNC_LOGGER] Pin failed: ", strerror(result), '\n');
    }
  }

  // NOTE: do NOT set_hot_path_mode(true) — this thread does write(2) I/O.

  while (!stop_.load(std::memory_order_relaxed)) {
    const auto count = drain_all();

    if (count == 0) {
      // Nothing to drain — backoff with _mm_pause() to reduce power and
      // avoid saturating the memory bus. 64 iterations ≈ 640ns on modern x86.
      for (int i = 0; i < 64; ++i) {
        _mm_pause();
      }
    }
  }

  // Final drain: flush remaining entries after stop flag is set.
  // Hot threads have already exited at this point.
  drain_all();
}

std::size_t AsyncLogger::drain_all() noexcept {
  constexpr std::size_t kDrainBatch = 128;
  // Per-queue batch buffers. Stack cost: kMaxQueues * 128 * 128B = 64KB.
  LogEntry batches[kMaxQueues][kDrainBatch];
  std::size_t counts[kMaxQueues]{};
  std::size_t indices[kMaxQueues]{};
  char line_buf[512];
  std::size_t total = 0;

  // Phase 1: drain all queues into separate batch buffers.
  for (std::size_t q = 0; q < num_queues_; ++q) {
    counts[q] = queues_[q]->drain(batches[q]);
    total += counts[q];
  }

  if (total == 0) {
    return 0;
  }

  // Phase 2: N-way merge by tsc_timestamp.
  // For small N (2-4 queues), linear scan to find the minimum is optimal —
  // a min-heap would add overhead for such a small number of queues.
  std::size_t written = 0;
  while (written < total) {
    // Find the queue with the smallest tsc_timestamp at its current index.
    std::size_t best = kMaxQueues; // sentinel
    std::uint64_t best_tsc = UINT64_MAX;
    for (std::size_t q = 0; q < num_queues_; ++q) {
      if (indices[q] < counts[q] &&
          batches[q][indices[q]].tsc_timestamp < best_tsc) {
        best_tsc = batches[q][indices[q]].tsc_timestamp;
        best = q;
      }
    }
    auto len = format_entry(batches[best][indices[best]], line_buf,
                            sizeof(line_buf));
    sys_write(log_fd_, line_buf, len);
    ++indices[best];
    ++written;
  }

  entries_written_.fetch_add(total, std::memory_order_relaxed);
  return total;
}

// =============================================================================
// Text Formatting
// =============================================================================

std::size_t AsyncLogger::format_entry(const LogEntry &entry, char *buf,
                                      std::size_t buf_size) const noexcept {
  // Format: "tsc_ns THREAD LEVEL EVENT  key=val key=val ...\n"
  std::size_t pos = 0;
  const auto cap = buf_size;

  // TSC timestamp → nanoseconds.
  const auto ns = static_cast<std::uint64_t>(tsc_cal_.to_ns(entry.tsc_timestamp));
  pos = append_u64(buf, pos, cap, ns);
  buf[pos++] = ' ';

  // Thread name.
  const auto tid = std::min(entry.thread_id, static_cast<std::uint16_t>(3));
  pos = append_str(buf, pos, cap, kThreadNames[tid]);
  buf[pos++] = ' ';

  // Level.
  pos = append_str(buf, pos, cap,
                   kLevelNames[static_cast<std::uint8_t>(entry.level)]);
  buf[pos++] = ' ';

  // Event type.
  pos = append_str(buf, pos, cap,
                   kEventTypeNames[static_cast<std::uint8_t>(entry.event_type)]);
  buf[pos++] = ' ';
  buf[pos++] = ' ';

  // Event-specific fields.
  switch (entry.event_type) {
  case LogEventType::kLatency: {
    pos = append_str(buf, pos, cap, "stage="sv);
    pos = append_str(
        buf, pos, cap,
        kLatencyStageNames[static_cast<std::uint8_t>(entry.latency.stage)]);
    pos = append_str(buf, pos, cap, " cycles="sv);
    pos = append_u64(buf, pos, cap, entry.latency.cycles);
    pos = append_str(buf, pos, cap, " ns="sv);
    const auto lat_ns =
        static_cast<std::uint64_t>(tsc_cal_.to_ns(entry.latency.cycles));
    pos = append_u64(buf, pos, cap, lat_ns);
    break;
  }
  case LogEventType::kOrder: {
    pos = append_str(buf, pos, cap, "event="sv);
    pos = append_str(
        buf, pos, cap,
        kOrderEventNames[static_cast<std::uint8_t>(entry.order.sub_type)]);
    pos = append_str(buf, pos, cap, " sym="sv);
    pos = append_u32(buf, pos, cap, entry.order.symbol_id);
    pos = append_str(buf, pos, cap, " side="sv);
    pos = append_str(buf, pos, cap,
                     entry.order.side < 2 ? kSideNames[entry.order.side]
                                          : "?"sv);
    pos = append_str(buf, pos, cap, " price="sv);
    pos = append_i64(buf, pos, cap, entry.order.price);
    pos = append_str(buf, pos, cap, " qty="sv);
    pos = append_u32(buf, pos, cap, entry.order.qty);
    pos = append_str(buf, pos, cap, " oid="sv);
    pos = append_u64(buf, pos, cap, entry.order.client_order_id);
    if (entry.order.exchange_order_id != 0) {
      pos = append_str(buf, pos, cap, " xoid="sv);
      pos = append_u64(buf, pos, cap, entry.order.exchange_order_id);
    }
    if (entry.order.remaining_qty != 0) {
      pos = append_str(buf, pos, cap, " rem="sv);
      pos = append_u32(buf, pos, cap, entry.order.remaining_qty);
    }
    break;
  }
  case LogEventType::kMarketData: {
    pos = append_str(buf, pos, cap, "seq="sv);
    pos = append_u64(buf, pos, cap, entry.market_data.seq_num);
    pos = append_str(buf, pos, cap, " sym="sv);
    pos = append_u32(buf, pos, cap, entry.market_data.symbol_id);
    pos = append_str(buf, pos, cap, " side="sv);
    pos = append_str(buf, pos, cap,
                     entry.market_data.side < 2
                         ? kSideNames[entry.market_data.side]
                         : "?"sv);
    pos = append_str(buf, pos, cap, " price="sv);
    pos = append_i64(buf, pos, cap, entry.market_data.price);
    pos = append_str(buf, pos, cap, " qty="sv);
    pos = append_u32(buf, pos, cap, entry.market_data.qty);
    if (entry.market_data.gap_size > 0) {
      pos = append_str(buf, pos, cap, " gap="sv);
      pos = append_u32(buf, pos, cap, entry.market_data.gap_size);
    }
    break;
  }
  case LogEventType::kConnection: {
    pos = append_str(buf, pos, cap, "event="sv);
    pos = append_str(buf, pos, cap,
                     kConnectionEventNames[static_cast<std::uint8_t>(
                         entry.connection.sub_type)]);
    if (entry.connection.attempt > 0) {
      pos = append_str(buf, pos, cap, " attempt="sv);
      pos = append_u32(buf, pos, cap, entry.connection.attempt);
    }
    if (entry.connection.rtt_ns > 0) {
      pos = append_str(buf, pos, cap, " rtt_ns="sv);
      pos = append_i64(buf, pos, cap, entry.connection.rtt_ns);
    }
    break;
  }
  case LogEventType::kText: {
    pos = append_str(buf, pos, cap, R"(msg=")"sv);
    const auto msg_len = ::strnlen(entry.text.msg, 111);
    pos = append_str(buf, pos, cap,
                     std::string_view(entry.text.msg, msg_len));
    if (pos < cap - 1) {
      buf[pos++] = '"';
    }
    break;
  }
  }

  // Newline.
  if (pos < cap - 1) {
    buf[pos++] = '\n';
  }

  return pos;
}

} // namespace mk::sys::log
