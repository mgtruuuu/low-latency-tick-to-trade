/**
 * @file md_types.hpp
 * @brief Shared market data types for MD feed and Strategy threads.
 *
 * Contains the cross-thread data contracts used by the SPSC queue
 * between MdFeedThread and StrategyThread, plus the MD feed epoll
 * event source for O(1) dispatch.
 */

#pragma once

#include "shared/protocol.hpp"

#include "sys/memory/spsc_queue.hpp"

#include <cstdint>

namespace mk::app {

// -- SPSC queue element --

/// Carries a parsed MarketDataUpdate from the MD thread to the Strategy
/// thread, along with two recv-time timestamps:
///   - recv_tsc:        TSC at recvmmsg() return (TSC-domain Tick-to-Trade).
///   - kernel_recv_ns:  CLOCK_REALTIME ns from SO_TIMESTAMPNS cmsg (kernel
///                      ingress; closer to wire-to-wire). 0 if cmsg missing
///                      or SO_TIMESTAMPNS disabled — caller should skip the
///                      kernel-RX metric for that datagram.
struct QueuedUpdate {
  MarketDataUpdate update;
  std::uint64_t recv_tsc{0};      // rdtsc() at recvmmsg time on MD thread
  std::int64_t kernel_recv_ns{0}; // CLOCK_REALTIME ns at kernel packet ingress
};

/// SPSC queue type for MD -> Strategy thread communication.
/// 1024 capacity handles burst of up to 1K in-flight updates.
/// Uses SPSCQueue (runtime capacity) instead of FixedSPSCQueue so the backing
/// buffer lives in an MmapRegion — huge-page-backed, NUMA-bound, prefaulted.
using MdToStrategyQueue = sys::memory::SPSCQueue<QueuedUpdate>;

// -- MD feed epoll types --

/// MD feed slot identifiers.
/// Used for epoll events array sizing (kCount) and slot identification.
enum class MdEpollSlot : std::uint32_t { kIncrementalA, kIncrementalB, kCount };

/// Epoll event source for the MD feed loop.
/// Stored in epoll_event.data.ptr — on event, cast back to MdFeedSource*
/// to get the fd and slot directly. No fd comparison or switch needed.
/// This is the standard production pattern for epoll-based event loops.
struct MdFeedSource {
  // -- Identity (set at construction, immutable) --
  int fd{-1};
  MdEpollSlot slot{MdEpollSlot::kIncrementalA};

  // -- Per-feed diagnostic counters --
  // Only the MD thread writes; main thread reads after join() — no atomics
  // needed.
  struct Stats {
    std::uint64_t packets{0};
    std::uint64_t bytes{0};
    /// `recvmmsg()` returned the full batch (rc == batch_size). It is a
    /// full-batch indicator, not a drop count and not proof of backlog —
    /// with batch_size == 1 every successful recv would bump it. At the
    /// production batch_size (currently 64), a sustained nonzero value
    /// suggests evaluating a larger batch under load. Increments in
    /// md_feed_thread.hpp inside the recvmmsg loop.
    std::uint64_t recv_batch_full{0};
    std::uint64_t queue_drops{0}; // SPSC push failed (strategy behind)
  } stats;
};

} // namespace mk::app
