/**
 * @file mmap_utils.cpp
 * @brief Implementation of mapping-policy-aware mmap allocation helpers.
 */

#include "memory/mmap_utils_internal.hpp"

#include "sys/log/signal_logger.hpp"

#include <cstdlib>
#include <optional>
#include <sys/mman.h>

namespace {

using mk::sys::memory::AllocationConfig;
using mk::sys::memory::FailureMode;
using mk::sys::memory::PageMappingMode;
using mk::sys::memory::MmapRegion;
using mk::sys::memory::PrefaultPolicy;
using mk::sys::memory::RegionIntent;
using mk::sys::memory::RegionIntentConfig;

enum class MappingPath : std::uint8_t {
  kExplicitHugePages,
  kTransparentHugePages,
  kRegularPages,
};

struct MappingResult {
  MmapRegion region;
  MappingPath path;
};

[[nodiscard]] bool is_numa_requested(const AllocationConfig &cfg) noexcept {
  return cfg.numa_node >= 0;
}

[[nodiscard]] PrefaultPolicy
initial_prefault_policy(const AllocationConfig &cfg, bool numa) noexcept {
  return numa ? PrefaultPolicy::kNone : cfg.pf;
}

[[nodiscard]] bool initial_lock_enabled(const AllocationConfig &cfg,
                                        bool numa) noexcept {
  return numa ? false : cfg.lock_pages;
}

[[nodiscard]] bool is_write_touch_policy(PrefaultPolicy pf) noexcept {
  return pf == PrefaultPolicy::kPopulateWrite ||
         pf == PrefaultPolicy::kManualWrite;
}

[[nodiscard]] bool needs_deferred_prefault(const AllocationConfig &cfg,
                                           bool numa,
                                           MappingPath path) noexcept {
  return (numa || path == MappingPath::kTransparentHugePages) &&
         cfg.pf != PrefaultPolicy::kNone;
}

[[nodiscard]] bool needs_deferred_lock(const AllocationConfig &cfg, bool numa,
                                       MappingPath path) noexcept {
  return (numa || path == MappingPath::kTransparentHugePages) && cfg.lock_pages;
}

[[nodiscard]] bool is_fail_fast(const AllocationConfig &cfg) noexcept {
  return cfg.failure_mode == FailureMode::kFailFast;
}

[[nodiscard]] AllocationConfig
build_allocation_config(const RegionIntentConfig &cfg,
                        PrefaultPolicy prefault) noexcept {
  return AllocationConfig{
      .size = cfg.size,
      .pf = prefault,
      .numa_node = cfg.numa_node,
      .lock_pages = cfg.lock_pages,
      .page_mapping_mode = cfg.page_mapping_mode,
      .huge_page_size = cfg.huge_page_size,
      .failure_mode = cfg.failure_mode,
  };
}

/// Intent-based page mapping mode selection.
/// kCold uses regular pages to avoid consuming the hugepage pool.
[[nodiscard]] PageMappingMode
page_mapping_mode_for_intent(RegionIntent intent,
                             PageMappingMode configured) noexcept {
  switch (intent) {
  case RegionIntent::kHotRw:
  case RegionIntent::kReadMostly:
    return configured; // respect user's choice
  case RegionIntent::kCold:
    return PageMappingMode::kRegularPages;
  }

  mk::sys::log::signal_log("[Critical] invalid RegionIntent=",
                           static_cast<int>(intent), '\n');
  std::abort();
}

[[nodiscard]] PrefaultPolicy
prefault_policy_for_intent(RegionIntent intent) noexcept {
  switch (intent) {
  case RegionIntent::kHotRw:
    return PrefaultPolicy::kPopulateWrite;
  case RegionIntent::kReadMostly:
    return PrefaultPolicy::kPopulateRead;
  case RegionIntent::kCold:
    return PrefaultPolicy::kNone;
  }

  // Defensive fail-fast for invalid enum values.
  mk::sys::log::signal_log("[Critical] invalid RegionIntent=",
                           static_cast<int>(intent), '\n');
  std::abort();
}

[[nodiscard]] std::optional<MappingResult>
try_map_explicit_huge_pages(const AllocationConfig &cfg, PrefaultPolicy alloc_pf,
                            bool alloc_lock) noexcept {
  auto explicit_region = MmapRegion::allocate_huge_pages(
      cfg.size, cfg.huge_page_size, alloc_pf, alloc_lock);
  if (!explicit_region) {
    return std::nullopt;
  }

  return MappingResult{
      .region = std::move(*explicit_region),
      .path = MappingPath::kExplicitHugePages,
  };
}

[[nodiscard]] std::optional<MappingResult>
try_map_thp_fallback(const AllocationConfig &cfg) noexcept {
  // THP mapping path: anonymous mmap + MADV_HUGEPAGE hint.
  // Prefault/lock are intentionally deferred so MADV_HUGEPAGE is set
  // before first fault.
  auto thp_region =
      MmapRegion::allocate_anonymous(cfg.size, PrefaultPolicy::kNone, false);
  if (!thp_region) {
    return std::nullopt;
  }

  // Best-effort THP hint. Continue even if madvise fails.
  if (!thp_region->advise(MADV_HUGEPAGE)) {
    mk::sys::log::signal_log("[Warn] MADV_HUGEPAGE failed, size=", cfg.size,
                             " — THP hint is best-effort; continuing\n");
  }

  return MappingResult{
      .region = std::move(*thp_region),
      .path = MappingPath::kTransparentHugePages,
  };
}

[[nodiscard]] std::optional<MappingResult>
map_hugepage_region(const AllocationConfig &cfg, PrefaultPolicy alloc_pf,
                    bool alloc_lock) noexcept {
  switch (cfg.page_mapping_mode) {
  case PageMappingMode::kRegularPages: {
    // Plain anonymous mmap, no huge-page attempt.
    auto region =
        MmapRegion::allocate_anonymous(cfg.size, alloc_pf, alloc_lock);
    if (!region) {
      return std::nullopt;
    }
    return MappingResult{
        .region = std::move(*region),
        .path = MappingPath::kRegularPages,
    };
  }
  case PageMappingMode::kThpOnly:
    // Skip explicit hugetlb and map directly via THP-backed path.
    return try_map_thp_fallback(cfg);
  case PageMappingMode::kExplicitOnly:
  case PageMappingMode::kExplicitThenThp: {
    // Explicit huge-page path: try hugetlb mapping first.
    auto explicit_mapped =
        try_map_explicit_huge_pages(cfg, alloc_pf, alloc_lock);
    if (explicit_mapped) {
      return explicit_mapped;
    }
    if (cfg.page_mapping_mode == PageMappingMode::kExplicitOnly) {
      mk::sys::log::signal_log(
          "[Warn] explicit huge-page allocation failed; "
          "PageMappingMode::kExplicitOnly disables THP fallback, size=",
          cfg.size, '\n');
      return std::nullopt;
    }

    // Fallback path for kExplicitThenThp.
    return try_map_thp_fallback(cfg);
  }
  }

  // Defensive fail-fast for invalid enum values.
  mk::sys::log::signal_log("[Critical] invalid PageMappingMode=",
                           static_cast<int>(cfg.page_mapping_mode), '\n');
  std::abort();
}

[[nodiscard]] bool bind_numa_if_needed(MmapRegion &region,
                                       const AllocationConfig &cfg,
                                       bool numa) noexcept {
  if (!numa) {
    return true;
  }
  if (!region.bind_numa_node(cfg.numa_node)) {
    if (is_fail_fast(cfg)) {
      mk::sys::log::signal_log("[Error] mbind failed, node=", cfg.numa_node,
                               " size=", cfg.size,
                               " — FailureMode::kFailFast requested\n");
      return false;
    }
    mk::sys::log::signal_log("[Warn] mbind failed, node=", cfg.numa_node,
                             " size=", cfg.size,
                             " — pages may fault on wrong NUMA node\n");
  }
  return true;
}

[[nodiscard]] bool prefault_if_needed(MmapRegion &region,
                                      const AllocationConfig &cfg, bool numa,
                                      MappingPath path) noexcept {
  if (!needs_deferred_prefault(cfg, numa, path)) {
    return true;
  }
  if (!region.prefault(is_write_touch_policy(cfg.pf))) {
    if (is_fail_fast(cfg)) {
      mk::sys::log::signal_log("[Error] prefault failed, size=", cfg.size,
                               " — FailureMode::kFailFast requested\n");
      return false;
    }
    mk::sys::log::signal_log("[Warn] prefault failed, size=", cfg.size,
                             " — first-touch page faults on hot path\n");
  }
  return true;
}

[[nodiscard]] bool lock_if_needed(MmapRegion &region,
                                  const AllocationConfig &cfg, bool numa,
                                  MappingPath path) noexcept {
  if (!needs_deferred_lock(cfg, numa, path)) {
    return true;
  }
  if (!region.lock()) {
    if (is_fail_fast(cfg)) {
      mk::sys::log::signal_log("[Error] mlock failed, size=", cfg.size,
                               " — FailureMode::kFailFast requested\n");
      return false;
    }
    mk::sys::log::signal_log("[Warn] mlock failed, size=", cfg.size,
                             " — pages may be swapped out. "
                             "Check RLIMIT_MEMLOCK (ulimit -l)\n");
  }
  return true;
}

} // namespace

namespace mk::sys::memory {

std::optional<MmapRegion>
try_allocate_with_hugepage_fallback(const AllocationConfig &cfg) noexcept {
  if (cfg.size == 0) {
    return std::nullopt;
  }

  const bool numa = is_numa_requested(cfg);

  // When NUMA binding is requested, defer prefault/lock until after mbind.
  // MAP_LOCKED can fault/lock pages during mmap; if that happens before mbind,
  // pages may land on the wrong NUMA node.
  // Same rationale for MAP_POPULATE: it can fault-in pages before mbind.
  const PrefaultPolicy alloc_pf = initial_prefault_policy(cfg, numa);
  const bool alloc_lock = initial_lock_enabled(cfg, numa);

  // Mapping phase:
  // select path by PageMappingMode
  // (regular pages, THP-only, explicit-only, or explicit-then-THP fallback).
  auto mapped = map_hugepage_region(cfg, alloc_pf, alloc_lock);
  if (!mapped) {
    return std::nullopt;
  }

  // Post-map policy phase (order-sensitive):
  //   mbind (optional) -> prefault (optional) -> lock (optional)
  //
  // Effective order by path:
  // - Regular pages: prefault/lock as requested (deferred only if NUMA is set)
  // - NUMA only: mbind -> prefault -> lock (faults land on the target node)
  // - THP fallback: MADV_HUGEPAGE (in mapping phase) -> prefault -> lock
  // - Both: MADV_HUGEPAGE (in mapping phase) -> mbind -> prefault -> lock
  //
  // Explicit huge pages without deferred policy: prefault/lock can already
  // be applied in allocate_huge_pages(), so these calls may become no-ops.
  if (!bind_numa_if_needed(mapped->region, cfg, numa)) {
    return std::nullopt;
  }
  if (!prefault_if_needed(mapped->region, cfg, numa, mapped->path)) {
    return std::nullopt;
  }
  if (!lock_if_needed(mapped->region, cfg, numa, mapped->path)) {
    return std::nullopt;
  }

  return std::move(mapped->region);
}

MmapRegion allocate_with_hugepage_fallback(const AllocationConfig &cfg) noexcept {
  auto region = try_allocate_with_hugepage_fallback(cfg);
  if (!region) {
    mk::sys::log::signal_log(
        "[Critical] allocate_with_hugepage_fallback failed, size=", cfg.size,
        '\n');
    std::abort();
  }
  return std::move(*region);
}

std::optional<MmapRegion> try_allocate_region(const RegionIntentConfig &cfg,
                                              RegionIntent intent) noexcept {
  auto alloc_cfg =
      build_allocation_config(cfg, prefault_policy_for_intent(intent));
  alloc_cfg.page_mapping_mode =
      page_mapping_mode_for_intent(intent, cfg.page_mapping_mode);
  return try_allocate_with_hugepage_fallback(alloc_cfg);
}

MmapRegion allocate_region(const RegionIntentConfig &cfg,
                           RegionIntent intent) noexcept {
  auto alloc_cfg =
      build_allocation_config(cfg, prefault_policy_for_intent(intent));
  alloc_cfg.page_mapping_mode =
      page_mapping_mode_for_intent(intent, cfg.page_mapping_mode);
  return allocate_with_hugepage_fallback(alloc_cfg);
}

} // namespace mk::sys::memory
