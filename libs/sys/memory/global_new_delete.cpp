/**
 * @file global_new_delete.cpp
 * @brief Global Memory Allocation Overrides for Hot Path Protection.
 *
 * This module provides custom global operator new/delete overloads
 * that integrate with a Hot Path Guard mechanism to prevent dynamic memory
 * allocations during critical execution paths.
 * - Uses "sys/log/signal_logger.hpp" for allocation-free, type-safe logging.
 * - Prevents infinite recursion (reentrancy) during crash reporting.
 */

#include "global_new_delete.hpp"
#include "sys/log/signal_logger.hpp"       // Signal-safe Logger
#include "sys/thread/hot_path_control.hpp" // set_hot_path_mode(), is_hot_path_mode()

#include <cstdlib> // posix_memalign

// =============================================================================
// INTERNAL IMPLEMENTATION (Hidden from the global scope)
// =============================================================================
namespace {

/**
 * @brief Enum to classify the source of the allocation request.
 */
enum class AllocType {
  kScalar,        // operator new
  kArray,         // operator new[]
  kAlignedScalar, // operator new (align_val_t)
  kAlignedArray   // operator new[] (align_val_t)
};

/**
 * @brief Helper to convert AllocType to string_view for the logger.
 * Returns std::string_view to avoid heap allocation (std::string).
 */
constexpr std::string_view get_type_sv(AllocType type) {
  switch (type) {
  case AllocType::kScalar:
    return "Scalar";
  case AllocType::kArray:
    return "Array";
  case AllocType::kAlignedScalar:
    return "Aligned Scalar";
  case AllocType::kAlignedArray:
    return "Aligned Array";
  default:
    return "Unknown";
  }
}

/**
 * @brief The Central Allocation Logic [Core 1]
 * All 'new' operators delegate to this function.
 */
void *alloc_impl(std::size_t size, std::align_val_t alignment, AllocType type) {
  // 1. Hot Path Guard Check
#ifndef NDEBUG
  if (mk::sys::thread::is_hot_path_mode()) {
    // [SAFE LOGGING] Using mk::sys::log::log
    // No heap allocation, no format string parsing overhead.
    mk::sys::log::signal_log(
        "[FATAL] Hot Path Allocation Violation! Type: ", get_type_sv(type),
        ", Size: ", size,
        " bytes, Align: ", static_cast<std::size_t>(alignment),
        ". Aborting.\n");

    std::abort();
  }
#endif

  // 2. Allocation Strategy
  void *ptr = nullptr;
  auto align_size = static_cast<size_t>(alignment);

  if (align_size <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
    // [jemalloc Integration Point]
    // If linked with -ljemalloc, this standard malloc is hijacked by jemalloc.
    ptr = std::malloc(size); // NOLINT(cppcoreguidelines-no-malloc)
  } else {

    // POSIX aligned allocation
    if (posix_memalign(&ptr, align_size, size) != 0) { // NOLINT(cppcoreguidelines-no-malloc)
      ptr = nullptr;
    }
  }

  // 3. Error Handling (HFT Style: Abort on OOM)
  if (!ptr) {
    mk::sys::log::signal_log("[FATAL] Out of Memory in ", get_type_sv(type),
                             " allocation. Size: ", size, "\n");
    std::abort();
  }

  return ptr;
}

/**
 * @brief The Central Deallocation Logic [Core 2]
 * All 'delete' operators delegate to this function.
 */
void free_impl(void *ptr, [[maybe_unused]] std::align_val_t alignment,
               AllocType type) {
  // 1. Hot Path Guard Check
  // Deallocation is also strictly forbidden in Hot Paths (potential locking).
#ifndef NDEBUG
  if (mk::sys::thread::is_hot_path_mode()) {
    mk::sys::log::signal_log("[FATAL] Hot Path Deallocation Violation! Type: ",
                             get_type_sv(type), ". Aborting.\n");
    std::abort();
  }
#endif

  if (!ptr) {
    return;
  }

  std::free(ptr); // NOLINT(cppcoreguidelines-no-malloc)
}

} // end anonymous namespace

// =============================================================================
// LINKER ANCHOR IMPLEMENTATION (Here is the best place!)
// =============================================================================
namespace mk::sys::memory {

void install_global_memory_guard() {
  // This function intentionally does nothing.
  // Its sole purpose is to create a strong symbol reference
  // so the linker pulls in this entire translation unit.
  const volatile bool loaded = true;
  (void)loaded;
}

} // namespace mk::sys::memory

// =============================================================================
// GLOBAL OPERATOR OVERRIDES (The "16 Shells")
// =============================================================================

// -----------------------------------------------------------------------------
// 1. Scalar Operators (new / delete)
// -----------------------------------------------------------------------------
void *operator new(std::size_t size) {
  return alloc_impl(size, std::align_val_t{__STDCPP_DEFAULT_NEW_ALIGNMENT__},
                    AllocType::kScalar);
}

void operator delete(void *p) noexcept {
  free_impl(p, std::align_val_t{__STDCPP_DEFAULT_NEW_ALIGNMENT__},
            AllocType::kScalar);
}

void operator delete(void *p, [[maybe_unused]] std::size_t size) noexcept {
  free_impl(p, std::align_val_t{__STDCPP_DEFAULT_NEW_ALIGNMENT__},
            AllocType::kScalar);
}

// -----------------------------------------------------------------------------
// 2. Array Operators (new[] / delete[])
// -----------------------------------------------------------------------------
void *operator new[](std::size_t size) {
  return alloc_impl(size, std::align_val_t{__STDCPP_DEFAULT_NEW_ALIGNMENT__},
                    AllocType::kArray);
}

void operator delete[](void *p) noexcept {
  free_impl(p, std::align_val_t{__STDCPP_DEFAULT_NEW_ALIGNMENT__},
            AllocType::kArray);
}

void operator delete[](void *p, [[maybe_unused]] std::size_t size) noexcept {
  free_impl(p, std::align_val_t{__STDCPP_DEFAULT_NEW_ALIGNMENT__},
            AllocType::kArray);
}

// -----------------------------------------------------------------------------
// 3. Aligned Scalar Operators (new ... align_val_t)
// -----------------------------------------------------------------------------
void *operator new(std::size_t size, std::align_val_t al) {
  return alloc_impl(size, al, AllocType::kAlignedScalar);
}

void operator delete(void *p, std::align_val_t al) noexcept {
  free_impl(p, al, AllocType::kAlignedScalar);
}

void operator delete(void *p, [[maybe_unused]] std::size_t size,
                     std::align_val_t al) noexcept {
  free_impl(p, al, AllocType::kAlignedScalar);
}

// -----------------------------------------------------------------------------
// 4. Aligned Array Operators (new[] ... align_val_t)
// -----------------------------------------------------------------------------
void *operator new[](std::size_t size, std::align_val_t al) {
  return alloc_impl(size, al, AllocType::kAlignedArray);
}

void operator delete[](void *p, std::align_val_t al) noexcept {
  free_impl(p, al, AllocType::kAlignedArray);
}

void operator delete[](void *p, [[maybe_unused]] std::size_t size,
                       std::align_val_t al) noexcept {
  free_impl(p, al, AllocType::kAlignedArray);
}

// -----------------------------------------------------------------------------
// 5. Nothrow Wrappers
// NOTE: These intentionally delegate to the throwing versions, which abort()
// on OOM rather than returning nullptr. In HFT, OOM is unrecoverable —
// fail-fast is preferred over silent nullptr propagation.
// -----------------------------------------------------------------------------
void *operator new(std::size_t size, const std::nothrow_t & /*unused*/) noexcept {
  return operator new(size);
}

void *operator new[](std::size_t size, const std::nothrow_t & /*unused*/) noexcept {
  return operator new[](size);
}

void *operator new(std::size_t size, std::align_val_t al,
                   const std::nothrow_t & /*unused*/) noexcept {
  return operator new(size, al);
}

void *operator new[](std::size_t size, std::align_val_t al,
                     const std::nothrow_t & /*unused*/) noexcept {
  return operator new[](size, al);
}