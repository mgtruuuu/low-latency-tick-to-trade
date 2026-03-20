/**
 * @file md_feed_thread.hpp
 * @brief MD feed thread — receives UDP multicast market data, parses via
 *        FeedHandler, pushes QueuedUpdate to the Strategy thread via SPSC.
 *
 * Lifecycle: construct (cold) -> start() (spawns thread) -> join().
 * The thread exits when the external stop flag becomes non-zero.
 *
 * Design:
 *   - Header-only, zero allocation on hot path.
 *   - Owns std::thread. Non-copyable, non-movable (thread + references).
 *   - Takes references to externally-owned state (epoll, feed_handler,
 *     tracker, md_ctx, md_queue, log_queue). Caller manages lifetimes.
 *   - stop_flag is a reference to std::atomic_flag owned by main.cpp.
 */

#pragma once

#include "feed_handler.hpp"
#include "latency_tracker.hpp"
#include "md_ctx.hpp"
#include "md_types.hpp"

#include "shared/protocol.hpp"

#include "net/epoll_wrapper.hpp"

#include "sys/log/async_logger.hpp"
#include "sys/log/log_macros.hpp"
#include "sys/log/signal_logger.hpp"
#include "sys/nano_clock.hpp"
#include "sys/thread/affinity.hpp"
#include "sys/thread/hot_path_control.hpp"

#include <atomic>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <thread>

namespace mk::app {

class MdFeedThread {
public:
  /// Construction parameters. All references must outlive the MdFeedThread.
  struct Config {
    net::EpollWrapper &epoll;
    FeedHandler &feed_handler;
    LatencyTracker &tracker;
    MdCtx &md_ctx;
    MdToStrategyQueue &md_queue;
    sys::log::LogQueue &log_queue;
    std::atomic_flag &stop_flag;
    std::int32_t pin_core_md;
    std::uint32_t max_symbols;
  };

  explicit MdFeedThread(const Config &cfg) noexcept
      : epoll_(cfg.epoll), feed_handler_(cfg.feed_handler),
        tracker_(cfg.tracker), md_ctx_(cfg.md_ctx), md_queue_(cfg.md_queue),
        log_queue_(cfg.log_queue), stop_flag_(cfg.stop_flag),
        pin_core_md_(cfg.pin_core_md), max_symbols_(cfg.max_symbols) {}

  // Non-copyable, non-movable (holds references + owns thread).
  MdFeedThread(const MdFeedThread &) = delete;
  MdFeedThread &operator=(const MdFeedThread &) = delete;
  MdFeedThread(MdFeedThread &&) = delete;
  MdFeedThread &operator=(MdFeedThread &&) = delete;

  ~MdFeedThread() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  /// Spawn the MD thread. Must be called exactly once.
  void start() { thread_ = std::thread(&MdFeedThread::run, this); }

  /// Block until the MD thread exits.
  void join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

private:
  /// The event loop — runs on the spawned thread.
  void run() noexcept {
    // Pin to the MD-dedicated core.
    if (pin_core_md_ >= 0) {
      auto result = sys::thread::pin_current_thread(
          static_cast<std::uint32_t>(pin_core_md_));
      if (result == 0) {
        sys::log::signal_log("[MD] Pinned to core ", pin_core_md_, '\n');
      } else {
        sys::log::signal_log("[MD] WARNING: pin failed: ", strerror(result),
                             '\n');
      }
    }

    sys::thread::set_hot_path_mode(true);
    sys::log::signal_log("[MD] Entering feed loop\n");

    constexpr auto kSlotCount = static_cast<std::size_t>(MdEpollSlot::kCount);
    std::array<struct epoll_event, kSlotCount> events{};
    const unsigned int batch_size = md_ctx_.batch_size;
    auto *msgvec = md_ctx_.msgvec;
    auto *iovecs = md_ctx_.iovecs;

    while (!stop_flag_.test(std::memory_order_relaxed)) {
      const int n = epoll_.wait(events, 0); // Busy-spin

      for (int i = 0; i < n; ++i) {
        if (!(events[i].events & EPOLLIN)) [[unlikely]] {
          continue;
        }

        auto *src = static_cast<MdFeedSource *>(events[i].data.ptr);
        assert(src != nullptr);
        const int fd = src->fd;

        // Edge-triggered: drain all available datagrams.
        while (true) {
          const int rc =
              ::recvmmsg(fd, msgvec, batch_size, MSG_DONTWAIT, nullptr);
          if (rc <= 0) {
            break;
          }
          src->stats.packets += static_cast<std::uint64_t>(rc);
          for (int m = 0; m < rc; ++m) {
            src->stats.bytes += msgvec[m].msg_len;
          }
          for (int m = 0; m < rc; ++m) {
            // Userspace rdtsc after recvmmsg — measures from parse start, not
            // kernel packet arrival. For true recv timestamps, use
            // SO_TIMESTAMPNS (kernel SW, ~tens of ns accuracy) or
            // SO_TIMESTAMPING with SOF_TIMESTAMPING_RX_HARDWARE (NIC HW,
            // ~single-digit ns, requires Solarflare/Mellanox class NICs).
            // Current placement is a practical trade-off: per-datagram rdtsc
            // inside the batch loop, since recvmmsg returns multiple datagrams
            // but we need one timestamp each.
            auto t0 = sys::rdtsc();

            const auto *buf = static_cast<const char *>(iovecs[m].iov_base);
            const auto len = static_cast<std::size_t>(msgvec[m].msg_len);

            MarketDataUpdate update;
            const bool parsed = feed_handler_.on_udp_data(buf, len, update);

#ifdef PROFILE_STAGES
            auto t1 = sys::rdtsc();
            tracker_.record_feed_parse(t1 - t0);
            (void)sys::log::log_latency(
                log_queue_, sys::log::kThreadIdMd,
                sys::log::LatencyStage::kFeedParse, t1 - t0, t0);
#else
            (void)tracker_;
#endif

            if (!parsed) {
              continue;
            }

            if (update.symbol_id == 0 || update.symbol_id > max_symbols_)
                [[unlikely]] {
              continue;
            }

            // Push to Strategy thread via SPSC queue.
            // If queue is full, drop the update (back-pressure signal).
            const QueuedUpdate queued{.update = update, .recv_tsc = t0};
            if (!md_queue_.try_push(queued)) [[unlikely]] {
              // Queue full — Strategy thread is falling behind.
              // Drop is acceptable: FeedHandler tracks gaps, and the
              // Strategy will process newer data when it catches up.
              ++src->stats.queue_drops;
            }

            // Log market data to async logger (binary push, ~5-10ns).
            (void)sys::log::log_market_data(
                log_queue_, sys::log::kThreadIdMd, sys::log::LogLevel::kInfo,
                update.seq_num, update.symbol_id,
                static_cast<std::uint8_t>(update.side), update.price,
                update.qty);
          }
        }
      }
    }

    sys::thread::set_hot_path_mode(false);
    sys::log::signal_log("[MD] Feed loop exited\n");
  }

  // -- References to externally-owned state --
  net::EpollWrapper &epoll_;
  FeedHandler &feed_handler_;
  LatencyTracker &tracker_;
  MdCtx &md_ctx_;
  MdToStrategyQueue &md_queue_;
  sys::log::LogQueue &log_queue_;
  std::atomic_flag &stop_flag_;

  // -- Configuration (value-copied at construction) --
  std::int32_t pin_core_md_;
  std::uint32_t max_symbols_;

  // -- Thread --
  std::thread thread_;
};

} // namespace mk::app
