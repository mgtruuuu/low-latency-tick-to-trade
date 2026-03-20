/**
 * @file mmap_utils_internal.hpp
 * @brief Internal/expert mmap allocation API for fine-grained prefault control.
 *
 * This header is intentionally separate from mmap_utils.hpp.
 * Use mmap_utils.hpp (intent API) for normal application code.
 */

#pragma once

#ifndef MK_SYS_INTERNAL_API
#error "mmap_utils_internal.hpp is internal. Use sys/memory/mmap_utils.hpp."
#endif

#include "sys/memory/mmap_utils.hpp"

#include <optional>

namespace mk::sys::memory {

/// Low-level configuration for mapping-policy-aware allocation.
/// Exposed via internal header for expert/manual prefault paths only.
struct AllocationConfig {
  std::size_t size{0};
  PrefaultPolicy pf{PrefaultPolicy::kPopulateWrite};
  int numa_node{-1}; // -1 = no NUMA binding.
  bool lock_pages{false};
  PageMappingMode page_mapping_mode{PageMappingMode::kExplicitThenThp};
  HugePageSize huge_page_size{HugePageSize::k2MB};
  FailureMode failure_mode{FailureMode::kWarnAndContinue};
};

/// Low-level allocation path:
///   - Mapping path selected by PageMappingMode
///     (explicit+THP fallback, explicit-only, THP-only, regular pages).
///   - Deferred mbind/prefault/lock in correct order.
[[nodiscard]] std::optional<MmapRegion>
try_allocate_with_hugepage_fallback(const AllocationConfig &cfg) noexcept;

/// Low-level allocation path that aborts on failure.
[[nodiscard]] MmapRegion
allocate_with_hugepage_fallback(const AllocationConfig &cfg) noexcept;

} // namespace mk::sys::memory
