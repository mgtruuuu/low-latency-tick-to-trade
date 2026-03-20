/**
 * @file async_logger.hpp
 * @brief Async logger with dedicated thread — 2x SPSC queues for HFT pipeline.
 *
 * Architecture: two SPSC queues (one per hot producer thread) drained by a
 * dedicated logger thread. Binary LogEntry structs are pushed on the hot path
 * (~5-10ns), formatted to text on the logger thread (cold path), and written
 * to file via write(2).
 *
 * HFT context:
 *   Industry standard pattern. No string formatting on the critical path.
 *   2x SPSC (not MPSC) eliminates contention between producer threads.
 *   Overflow policy: drop (try_push returns false). Never block producer.
 */

#pragma once

#include "sys/hardware_constants.hpp"
#include "sys/log/async_log_entry.hpp"
#include "sys/memory/mmap_region.hpp"
#include "sys/memory/spsc_queue.hpp"
#include "sys/nano_clock.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

namespace mk::sys::log {

/// Queue type alias — SPSCQueue<LogEntry> (non-owning, backed by MmapRegion).
using LogQueue = mk::sys::memory::SPSCQueue<LogEntry>;

/// Async logger: per-producer SPSC queues drained by a dedicated thread.
///
/// Entries from all queues are merged in TSC timestamp order before writing,
/// producing a globally time-ordered log file. Adding a new producer queue
/// requires: (1) add a LogQueue member, (2) register it in queues_[] in the
/// constructor, (3) add a public accessor.
///
/// Usage:
///   // Cold path (main):
///   AsyncLogger logger("pipeline.log", 4096, numa_node);
///   logger.start(pin_core_logger);
///
///   // Hot path (MD thread):
///   logger.md_queue().try_push(entry);
///
///   // Shutdown (main, after hot threads join):
///   logger.stop();
///
class AsyncLogger {
public:
  /// Maximum number of producer queues. Determines stack allocation in
  /// drain_all() — each queue gets a 128-entry batch buffer (128 * 128B = 16KB).
  /// 4 is sufficient for typical HFT pipelines (MD + Strategy + spare).
  /// Increase at compile time if more producer threads are added.
  static constexpr std::size_t kMaxQueues = 4;

  /// @param log_path   Output file path (O_WRONLY | O_CREAT | O_APPEND).
  /// @param capacity   SPSC queue capacity per producer (power-of-two).
  /// @param numa_node  NUMA node for queue memory binding (-1 = no binding).
  AsyncLogger(const char *log_path, std::uint32_t capacity = 4096,
              int numa_node = -1) noexcept;

  ~AsyncLogger() noexcept;

  // Non-copyable, non-movable (owns thread + fd).
  AsyncLogger(const AsyncLogger &) = delete;
  AsyncLogger &operator=(const AsyncLogger &) = delete;
  AsyncLogger(AsyncLogger &&) = delete;
  AsyncLogger &operator=(AsyncLogger &&) = delete;

  /// Start the logger thread. Optionally pin to a CPU core.
  void start(std::int32_t pin_core = -1) noexcept;

  /// Request stop and join the logger thread.
  /// Drains all remaining entries before returning.
  void stop() noexcept;

  /// Producer interface: SPSC queue for the MD thread.
  [[nodiscard]] LogQueue &md_queue() noexcept { return md_queue_; }

  /// Producer interface: SPSC queue for the Strategy thread.
  [[nodiscard]] LogQueue &strategy_queue() noexcept { return strategy_queue_; }

  /// Total entries written to file.
  [[nodiscard]] std::uint64_t entries_written() const noexcept {
    return entries_written_.load(std::memory_order_relaxed);
  }

private:
  /// Logger thread main function.
  void run(std::int32_t pin_core) noexcept;

  /// Drain all queues and write entries in TSC-sorted order.
  /// Returns total entries drained.
  std::size_t drain_all() noexcept;

  /// Format one LogEntry into a text line.
  /// @return Number of characters written (not including null terminator).
  std::size_t format_entry(const LogEntry &entry, char *buf,
                           std::size_t buf_size) const noexcept;

  // -- Data members --

  // MmapRegion backing stores (declared before queues for init order).
  memory::MmapRegion md_queue_mem_;
  memory::MmapRegion strategy_queue_mem_;

  LogQueue md_queue_;
  LogQueue strategy_queue_;

  // Generic queue array for N-way merge in drain_all().
  // Points into the named queue members above. Adding a new producer =
  // add a LogQueue member + register its pointer here.
  LogQueue *queues_[kMaxQueues]{};
  std::size_t num_queues_{0};

  alignas(kCacheLineSize) std::atomic<bool> stop_{false};

  int log_fd_{-1};
  std::thread thread_;
  TscCalibration tsc_cal_;

  std::atomic<std::uint64_t> entries_written_{0};
};

} // namespace mk::sys::log
