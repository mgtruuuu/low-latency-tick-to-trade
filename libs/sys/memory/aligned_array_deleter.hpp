/**
 * @file aligned_array_deleter.hpp
 * @brief Custom deleter for byte arrays allocated with explicit alignment.
 *
 * When allocating raw byte buffers with explicit alignment:
 *
 *   auto* p = new (std::align_val_t{64}) std::byte[size];
 *
 * the default unique_ptr deleter calls plain delete[], which does NOT pass
 * alignment to operator delete[]. This is a new-delete-type-mismatch
 * (undefined behavior, caught by ASan).
 *
 * AlignedArrayDeleter pairs the aligned operator new[] with the matching
 * aligned operator delete[], ensuring correct deallocation.
 *
 * Note: This is only needed for types whose alignof is NOT over-aligned
 * (e.g., std::byte with alignof == 1) but were allocated with explicit
 * alignment. For over-aligned types (alignas(64) struct Foo), the
 * compiler automatically selects the aligned delete forms since C++17.
 *
 * Usage:
 *
 *   constexpr std::size_t kAlign = 64;
 *   mk::sys::memory::AlignedByteArray<kAlign> buf{
 *       new (std::align_val_t{kAlign}) std::byte[size]};
 *   // buf automatically calls aligned delete[] on destruction
 */

#pragma once

#include <cstddef>
#include <memory>
#include <new>

namespace mk::sys::memory {

/// Deleter that calls aligned operator delete[] to match an allocation
/// made with aligned operator new[].
/// Alignment is a compile-time template parameter (zero runtime overhead).
template <std::size_t Align> struct AlignedArrayDeleter {
  void operator()(std::byte *p) const noexcept {
    ::operator delete[](p, std::align_val_t{Align});
  }
};

/// Convenience alias: unique_ptr<std::byte[]> with aligned delete[].
template <std::size_t Align = alignof(std::max_align_t)>
using AlignedByteArray =
    std::unique_ptr<std::byte[], AlignedArrayDeleter<Align>>;

} // namespace mk::sys::memory
