/**
 * @file mmap_utils.hpp
 * @brief Convenience allocation helpers built on MmapRegion.
 *
 * Encapsulates startup mapping policy used throughout the codebase
 * (OrderBook, ObjectPool, pipeline launcher):
 *   1. Choose mapping path by PageMappingMode
 *      (explicit huge pages + THP fallback, THP-only, or regular pages).
 *   2. Optionally NUMA-bind + prefault + lock in order-sensitive sequence.
 *
 * MmapRegion itself is the low-level RAII handle.
 * This file provides both:
 *   - try_* APIs: return std::optional on recoverable failure
 *   - allocate_* APIs: abort-on-failure convenience wrappers
 *
 * Usage-specific madvise hints (MADV_RANDOM, MADV_SEQUENTIAL, etc.)
 * are NOT applied here — callers apply them after allocation.
 */

#pragma once

#include "mmap_region.hpp"

#include <optional>

namespace mk::sys::memory {

enum class PageMappingMode : std::uint8_t {
  kExplicitThenThp, // Try MAP_HUGETLB first, then THP fallback.
  kExplicitOnly,    // MAP_HUGETLB only (fail if unavailable).
  kThpOnly,         // Skip MAP_HUGETLB, use THP path only.
  kRegularPages,    // Plain anonymous mmap, no huge page attempt.
};

enum class FailureMode : std::uint8_t {
  kWarnAndContinue, // Log warnings for non-critical step failures.
  kFailFast,        // Treat step failures as allocation failure.
};

/// High-level app-facing configuration (prefault policy is selected by intent).
struct RegionIntentConfig {
  std::size_t size{0};
  int numa_node{-1}; // -1 = no NUMA binding.
  bool lock_pages{false};
  PageMappingMode page_mapping_mode{PageMappingMode::kExplicitThenThp};
  HugePageSize huge_page_size{HugePageSize::k2MB};
  FailureMode failure_mode{FailureMode::kWarnAndContinue};
};

/// Usage intent for a memory region — determines prefault policy internally.
/// For manual prefault control, use AllocationConfig (internal API) directly.
enum class RegionIntent : std::uint8_t {
  kHotRw,      // Write-prefaulted. For hot-path read/write buffers.
  kReadMostly, // Read-prefaulted. For lookup tables, config data.
  kCold,       // No prefault. For cold-path, non-latency-critical regions.
};

/// Core intent API (long-term extension point).
[[nodiscard]] std::optional<MmapRegion>
try_allocate_region(const RegionIntentConfig &cfg, RegionIntent intent) noexcept;
[[nodiscard]] MmapRegion
allocate_region(const RegionIntentConfig &cfg, RegionIntent intent) noexcept;

/// Intent-based helpers (recommended for app code).
/// Thin wrappers over try_allocate_region/allocate_region for call-site clarity.
[[nodiscard]] inline std::optional<MmapRegion>
try_allocate_hot_rw_region(const RegionIntentConfig &cfg) noexcept {
  return try_allocate_region(cfg, RegionIntent::kHotRw);
}
[[nodiscard]] inline MmapRegion
allocate_hot_rw_region(const RegionIntentConfig &cfg) noexcept {
  return allocate_region(cfg, RegionIntent::kHotRw);
}

[[nodiscard]] inline std::optional<MmapRegion>
try_allocate_read_mostly_region(const RegionIntentConfig &cfg) noexcept {
  return try_allocate_region(cfg, RegionIntent::kReadMostly);
}
[[nodiscard]] inline MmapRegion
allocate_read_mostly_region(const RegionIntentConfig &cfg) noexcept {
  return allocate_region(cfg, RegionIntent::kReadMostly);
}

[[nodiscard]] inline std::optional<MmapRegion>
try_allocate_cold_region(const RegionIntentConfig &cfg) noexcept {
  return try_allocate_region(cfg, RegionIntent::kCold);
}
[[nodiscard]] inline MmapRegion
allocate_cold_region(const RegionIntentConfig &cfg) noexcept {
  return allocate_region(cfg, RegionIntent::kCold);
}

} // namespace mk::sys::memory
