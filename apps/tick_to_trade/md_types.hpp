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
/// thread, along with the rdtsc timestamp captured at UDP recv time.
/// 48 bytes — fits in one cache line.
struct QueuedUpdate {
  MarketDataUpdate update;
  std::uint64_t recv_tsc{0}; // rdtsc() at recvmmsg time on MD thread
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
    std::uint64_t datagrams_dropped{0}; // recvmmsg overflow (rc == batch_size)
    std::uint64_t queue_drops{0};       // SPSC push failed (strategy behind)
  } stats;
};

} // namespace mk::app
