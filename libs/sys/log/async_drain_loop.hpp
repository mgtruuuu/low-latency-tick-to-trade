/**
 * @file async_drain_loop.hpp
 * @brief Generic async drain loop — SPSC queue draining with dedicated thread.
 *
 * Template-based async logger infrastructure: drains N SPSC queues on a
 * dedicated thread, merges entries by TSC timestamp, formats via a
 * caller-provided functor, and writes to file via write(2).
 *
 * This is the generic infrastructure layer. Application-specific types
 * (entry structs, formatters, queue topology) live in the app layer.
 *
 * Ordering guarantee:
 *   Entries are TSC-sorted within each drain batch (128 entries per queue).
 *   Cross-batch ordering is best-effort — a later batch may contain entries
 *   with earlier timestamps than the previous batch's last entry. Per-queue
 *   FIFO is always preserved. This is standard for diagnostic loggers;
 *   strict global ordering requires a separate sequenced journal.
 *
 * @tparam EntryT      Trivially-copyable entry type with a `tsc_timestamp`
 *                     field (uint64_t). Must satisfy DrainableEntry concept.
 * @tparam FormatterT  Callable: (const EntryT&, const TscCalibration&,
 *                     std::span<char>) -> size_t. Returns bytes written.
 *
 * Memory ownership:
 *   Non-owning for queue buffers — queues are registered via register_queue().
 *   Owns: drain thread, log file descriptor, TscCalibration.
 */

#pragma once

#include "sys/hardware_constants.hpp"
#include "sys/log/signal_logger.hpp"
#include "sys/memory/spsc_queue.hpp"
#include "sys/nano_clock.hpp"
#include "sys/thread/affinity.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <span>
#include <thread>
#include <type_traits>
#include <unistd.h>

namespace mk::sys::log {

// =============================================================================
// Concepts
// =============================================================================

/// Entry type must be trivially copyable and have a uint64_t tsc_timestamp.
template <typename T>
concept DrainableEntry =
    std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T> &&
    requires(const T &e) {
      { e.tsc_timestamp } -> std::convertible_to<std::uint64_t>;
    };

/// Formatter callable: (entry, tsc_cal, buf_span) -> bytes_written.
template <typename F, typename T>
concept EntryFormatter =
    requires(const F &f, const T &entry, const TscCalibration &cal,
             std::span<char> buf) {
      { f(entry, cal, buf) } -> std::same_as<std::size_t>;
    };

// =============================================================================
// AsyncDrainLoop
// =============================================================================

template <DrainableEntry EntryT, typename FormatterT>
  requires EntryFormatter<FormatterT, EntryT>
class AsyncDrainLoop {
public:
  /// Maximum number of producer queues. Each queue gets a 128-entry batch
  /// buffer on the drain thread stack (128 * sizeof(EntryT) per queue).
  static constexpr std::size_t kMaxQueues = 4;

  using Queue = mk::sys::memory::SPSCQueue<EntryT>;

  /// @param log_path   Output file path (O_WRONLY | O_CREAT | O_APPEND).
  /// @param formatter  Callable for converting entries to text lines.
  explicit AsyncDrainLoop(const char *log_path,
                          FormatterT formatter = {}) noexcept
      : formatter_(formatter),
        log_fd_(
            ::open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644)),
        tsc_cal_(TscCalibration::calibrate()) {
    if (log_fd_ < 0) {
      signal_log("[DRAIN_LOOP] Failed to open log file: ", log_path,
                 " errno=", errno, '\n');
      std::abort();
    }
  }

  ~AsyncDrainLoop() noexcept {
    stop();
    if (log_fd_ >= 0) {
      ::close(log_fd_);
      log_fd_ = -1;
    }
  }

  // Non-copyable, non-movable (owns thread + fd).
  AsyncDrainLoop(const AsyncDrainLoop &) = delete;
  AsyncDrainLoop &operator=(const AsyncDrainLoop &) = delete;
  AsyncDrainLoop(AsyncDrainLoop &&) = delete;
  AsyncDrainLoop &operator=(AsyncDrainLoop &&) = delete;

  /// Register a producer queue for draining. Call before start().
  /// @return true if registered, false if kMaxQueues reached.
  bool register_queue(Queue *q) noexcept {
    if (num_queues_ >= kMaxQueues || q == nullptr) {
      return false;
    }
    queues_[num_queues_++] = q;
    return true;
  }

  /// Start the drain thread. Optionally pin to a CPU core.
  void start(std::int32_t pin_core = -1) noexcept {
    thread_ = std::thread(&AsyncDrainLoop::run, this, pin_core);
  }

  /// Request stop and join the drain thread.
  /// Drains all remaining entries before returning.
  void stop() noexcept {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  /// Total entries written to file.
  [[nodiscard]] std::uint64_t entries_written() const noexcept {
    return entries_written_.load(std::memory_order_relaxed);
  }

private:
  void run(std::int32_t pin_core) noexcept {
    if (pin_core >= 0) {
      auto result = mk::sys::thread::pin_current_thread(
          static_cast<std::uint32_t>(pin_core));
      if (result == 0) {
        signal_log("[DRAIN_LOOP] Pinned to core ", pin_core, '\n');
      } else {
        signal_log("[DRAIN_LOOP] Pin failed: ", strerror(result), '\n');
      }
    }

    while (!stop_.load(std::memory_order_relaxed)) {
      const auto count = drain_all();
      if (count == 0) {
        // Backoff with _mm_pause() to reduce power and avoid saturating
        // the memory bus. 64 iterations ≈ 640ns on modern x86.
        for (int i = 0; i < 64; ++i) {
          _mm_pause();
        }
      }
    }

    // Final drain: flush remaining entries after stop flag is set.
    drain_all();
  }

  std::size_t drain_all() noexcept {
    static constexpr std::size_t kDrainBatch = 128;
    EntryT batches[kMaxQueues][kDrainBatch]; // NOLINT(*-avoid-c-arrays) —
                                             // drain() requires C-array ref
    std::array<std::size_t, kMaxQueues> counts{};
    std::array<std::size_t, kMaxQueues> indices{};
    std::array<char, 512> line_buf{};
    std::size_t total = 0;

    // Phase 1: drain all queues into separate batch buffers.
    for (std::size_t q = 0; q < num_queues_; ++q) {
      counts[q] = queues_[q]->drain(batches[q], kDrainBatch);
      total += counts[q];
    }

    if (total == 0) {
      return 0;
    }

    // Phase 2: N-way merge by tsc_timestamp within this batch.
    //
    // This is a best-effort convenience for real-time readability (tail -f),
    // NOT a correctness requirement. Entries from different queues in this
    // batch are interleaved by TSC so the output is roughly chronological.
    // Cross-batch ordering is not guaranteed — a later batch may contain
    // entries with earlier timestamps. Strict global ordering is achieved
    // offline (sort by tsc_timestamp after collection).
    //
    // For small N (2-4 queues), linear scan is optimal.
    std::size_t written = 0;
    while (written < total) {
      std::size_t best = kMaxQueues; // sentinel
      std::uint64_t best_tsc = UINT64_MAX;
      for (std::size_t q = 0; q < num_queues_; ++q) {
        if (indices[q] < counts[q] &&
            batches[q][indices[q]].tsc_timestamp < best_tsc) {
          best_tsc = batches[q][indices[q]].tsc_timestamp;
          best = q;
        }
      }
      auto len = formatter_(batches[best][indices[best]], tsc_cal_, line_buf);
      // Clamp: formatter may return "desired" length exceeding buffer
      // (e.g., snprintf truncation). Never write past the buffer.
      if (len > line_buf.size()) {
        len = line_buf.size();
      }
      sys_write(log_fd_, line_buf.data(), len);
      ++indices[best];
      ++written;
    }

    entries_written_.fetch_add(total, std::memory_order_relaxed);
    return total;
  }

  // -- Data members (declaration order = initialization order) --

  // Initialized in constructor initializer list.
  FormatterT formatter_;
  int log_fd_{-1};
  TscCalibration tsc_cal_;

  // Default-initialized (populated via register_queue / start).
  std::array<Queue *, kMaxQueues> queues_{};
  std::size_t num_queues_{0};
  std::thread thread_;

  // Atomics — accessed concurrently by producer / drain threads.
  // stop_ is cache-line-isolated: producer threads poll via relaxed load.
  alignas(kCacheLineSize) std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> entries_written_{0};
};

} // namespace mk::sys::log
