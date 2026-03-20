/**
 * @file tick_data.hpp
 * @brief Binary tick file format for market data replay.
 *
 * Defines the on-disk layout for pre-generated BBO (Best Bid/Offer) snapshots.
 * The exchange reads this file via mmap and replays ticks through UDP
 * multicast, enabling deterministic, reproducible latency measurement.
 *
 * File layout:
 *   [TickFileHeader: 64 bytes]   <-- 1 cache line
 *   [Tick 0: 40 bytes]
 *   [Tick 1: 40 bytes]
 *   ...
 *   [Tick N-1: 40 bytes]
 *
 * Design:
 *   - Host byte order (little-endian on x86-64) for zero-copy mmap access.
 *     The exchange converts to network byte order (big-endian) when publishing
 *     via UDP — same as any real feed handler.
 *   - Naturally aligned (no #pragma pack). Tick is 8-byte aligned so
 *     mmap'd pointer arithmetic works without unaligned access.
 *   - Fixed-point pricing via algo::Price (int64_t, × 10000 in this project).
 *     No floating point anywhere in the hot path.
 */

#pragma once

#include "algo/trading_types.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace mk::app {

// ======================================================================
// File format constants
// ======================================================================

/// File magic: ASCII "MKTICK01" stored as little-endian uint64_t.
/// Validates that the file is a tick data file and not a random binary.
inline constexpr std::uint64_t kTickFileMagic = 0x3130'4B43'4954'4B4DULL;

/// File format version. Increment when Tick or TickFileHeader layout changes.
inline constexpr std::uint32_t kTickFileVersion = 1;

// ======================================================================
// Tick — one BBO snapshot for one symbol (40 bytes)
// ======================================================================
// Each tick represents: "at this timestamp, symbol X has this best bid/ask."
// The exchange publishes this as 2 UDP datagrams (bid update + ask update).

struct Tick {
  std::uint64_t timestamp_ns; // Nanoseconds since session start
  std::uint32_t symbol_id;    // 0-based symbol index (replay converts to 1-based wire id)
  std::uint32_t pad0;         // 8-byte alignment padding (zero-filled)
  algo::Price bid_price;      // Best bid price (fixed-point)
  algo::Price ask_price;      // Best ask price (fixed-point)
  algo::Qty bid_qty;          // Quantity at best bid
  algo::Qty ask_qty;          // Quantity at best ask
};

static_assert(sizeof(Tick) == 40, "Tick must be exactly 40 bytes");
static_assert(alignof(Tick) == 8, "Tick must be 8-byte aligned for mmap");

// ======================================================================
// TickFileHeader — file metadata (64 bytes = 1 cache line)
// ======================================================================

struct TickFileHeader {
  std::uint64_t magic;        // Must be kTickFileMagic
  std::uint32_t version;      // Must be kTickFileVersion
  std::uint32_t symbol_count; // Number of distinct symbols
  std::uint64_t tick_count;   // Total number of Tick records following header
  std::uint64_t session_start_ns; // Session start offset (nanos since midnight)
  std::uint64_t session_end_ns;   // Session end offset (nanos since midnight)
  std::uint8_t reserved[24];      // Future use (zero-filled)
};

static_assert(sizeof(TickFileHeader) == 64, "TickFileHeader must be 64 bytes");

// ======================================================================
// Helpers
// ======================================================================

/// Calculate file size for a given tick count.
constexpr std::size_t tick_file_size(std::uint64_t tick_count) noexcept {
  return sizeof(TickFileHeader) + (tick_count * sizeof(Tick));
}

/// Access tick array from a validated file buffer.
/// @pre base points to a valid TickFileHeader followed by tick data.
inline const Tick *tick_array(const void *base) noexcept {
  assert(base != nullptr);
  return reinterpret_cast<const Tick *>(static_cast<const std::byte *>(base) +
                                        sizeof(TickFileHeader));
}

} // namespace mk::app
