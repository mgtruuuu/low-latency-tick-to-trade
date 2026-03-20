/**
 * @file mmap_region.hpp
 * @brief RAII wrapper for mmap'd memory regions.
 *
 * Like ScopedFd manages file descriptors, MmapRegion manages mmap'd memory.
 * A reusable building block for:
 *   - Anonymous private mappings (general pre-allocation)
 *   - Explicit huge pages (MAP_HUGETLB) for hot-path pools
 *   - POSIX shared memory (shm_open + mmap) for IPC
 *   - File-backed mappings (mmap over an open fd)
 *
 * Design:
 *   - Move-only RAII ownership (like ScopedFd).
 *   - Named factory methods return std::optional — caller decides abort policy.
 *   - Invalid state sentinel: MAP_FAILED (see addr_ comment for rationale).
 *   - No exceptions. noexcept throughout.
 *   - errno preserved across signal_log calls (production pattern).
 *
 * @note This is a resource handle, NOT a concurrent data structure.
 *   Once constructed, data()/size() are safe for concurrent reads.
 *   Mutators (release/reset/protect/remap) require exclusive access.
 */

#pragma once

#ifndef __linux__
#error "mmap_region.hpp requires Linux (mmap, mremap, MAP_HUGETLB, SYS_mbind)"
#endif

#include <cstddef>
#include <cstdint>
#include <cstdlib> // std::abort
#include <optional>
#include <string_view>
#include <utility> // std::swap

#include <sys/mman.h> // MAP_FAILED, munmap, madvise, mlock, munlock, mprotect

namespace mk::sys::memory {

// =============================================================================
// Enums
// =============================================================================

/// Controls whether pages are populated at creation time (cold path)
/// to avoid first-touch page faults on the hot path.
///
/// Write variants (producer/owner): dirty pages, allocate physical frames.
/// Read variants (consumer): populate page tables without modifying data.
/// Use read variants for shared memory consumers to avoid overwriting
/// producer data.
enum class PrefaultPolicy : std::uint8_t {
  kNone,          ///< No prefaulting. Pages faulted on first access.
  kPopulateWrite, ///< MADV_POPULATE_WRITE (Linux 5.14+), fallback to manual
                  ///< write.
  kPopulateRead,  ///< MADV_POPULATE_READ  (Linux 5.14+), fallback to manual
                  ///< read.
  kManualWrite,   ///< Per-page write loop. Portable but slower.
  kManualRead,    ///< Per-page read loop. Portable, preserves existing data.
};

/// Huge page size selector for allocate_huge_pages().
enum class HugePageSize : std::uint8_t {
  k2MB, // NOLINT(readability-identifier-naming)
  k1GB, // NOLINT(readability-identifier-naming)
};

/// Mode for POSIX shared memory opening.
enum class ShmMode : std::uint8_t {
  kCreateOrOpen, ///< O_CREAT | O_RDWR (producer — creates if absent).
  kOpenExisting, ///< O_RDWR only (consumer — must already exist).
};

// =============================================================================
// MmapRegion
// =============================================================================

class MmapRegion {
  // MAP_FAILED ((void*)-1) is the invalid sentinel, not nullptr.
  // mmap() returns MAP_FAILED on error. nullptr (address 0) is technically
  // a valid mapping address, so we must use the OS-native error sentinel.
  void *addr_ = MAP_FAILED;
  std::size_t size_ = 0;

public:
  // ===========================================================================
  // Construction / Destruction
  // ===========================================================================

  /// Default-constructs in invalid state (like ScopedFd(-1)).
  MmapRegion() noexcept = default;

  ~MmapRegion() noexcept {
    if (addr_ != MAP_FAILED) {
      // munmap failure means we passed invalid arguments (double-free,
      // corrupted addr/size, or already-unmapped region). This is an
      // unrecoverable bug — abort immediately to prevent silent corruption.
      if (::munmap(addr_, size_) != 0) {
        std::abort();
      }
    }
  }

  // ===========================================================================
  // Move-only (unique ownership, like ScopedFd)
  // ===========================================================================

  MmapRegion(MmapRegion &&other) noexcept { swap(other); }

  MmapRegion &operator=(MmapRegion &&other) noexcept {
    // Move-construct temp from other (other becomes invalid), then swap
    // our state into temp. temp's destructor munmaps our old mapping
    // before this function returns — immediate cleanup, not deferred.
    MmapRegion tmp(std::move(other));
    swap(tmp);
    return *this;
  }

  MmapRegion(const MmapRegion &) = delete;
  MmapRegion &operator=(const MmapRegion &) = delete;

  // ===========================================================================
  // Observers
  // ===========================================================================

  /// Raw pointer to the mapped region. Returns nullptr if invalid.
  /// Translates MAP_FAILED → nullptr for external callers.
  [[nodiscard]] void *get() const noexcept {
    return (addr_ != MAP_FAILED) ? addr_ : nullptr;
  }

  /// Typed pointer for byte-level access. Returns nullptr if invalid.
  [[nodiscard]] std::byte *data() const noexcept {
    return static_cast<std::byte *>(get());
  }

  /// Size of the mapped region in bytes. 0 if invalid.
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  [[nodiscard]] bool is_valid() const noexcept { return addr_ != MAP_FAILED; }

  explicit operator bool() const noexcept { return is_valid(); }

  // ===========================================================================
  // Mutators
  // ===========================================================================

  /// Return type for release() — carries both the address and size
  /// needed for a manual munmap() call.
  struct RawRegion {
    void *addr;
    std::size_t size;
  };

  /// Releases ownership without unmapping. The caller becomes responsible
  /// for calling munmap(addr, size).
  /// Returns {MAP_FAILED, 0} if invalid — caller must check addr before
  /// calling munmap (munmap with MAP_FAILED returns EINVAL).
  [[nodiscard]] RawRegion release() noexcept {
    RawRegion r{.addr = addr_, .size = size_};
    addr_ = MAP_FAILED;
    size_ = 0;
    return r;
  }

  /// Unmaps the current region (if valid) and enters invalid state.
  void reset() noexcept {
    if (addr_ != MAP_FAILED) {
      if (::munmap(addr_, size_) != 0) {
        std::abort();
      }
    }
    addr_ = MAP_FAILED;
    size_ = 0;
  }

  void swap(MmapRegion &other) noexcept {
    std::swap(addr_, other.addr_);
    std::swap(size_, other.size_);
  }

  friend void swap(MmapRegion &a, MmapRegion &b) noexcept { a.swap(b); }

  // ===========================================================================
  // Memory Operations (cold path — call during initialization)
  // ===========================================================================

  /// Prefault pages to eliminate first-touch page faults on the hot path.
  /// @param write_touch true = write-fault (producer/owner, dirtying pages).
  ///                    false = read-fault (consumer, preserves existing data).
  /// @return true on success, false if the region is invalid.
  bool prefault(bool write_touch = true) noexcept;

  /// Wrapper around madvise(2). Returns true on success.
  /// Common advice values:
  ///   MADV_HUGEPAGE   — hint for transparent huge pages
  ///   MADV_SEQUENTIAL — sequential access pattern (enables aggressive
  ///   read-ahead) MADV_RANDOM     — random access pattern (disables
  ///   read-ahead, reduces
  ///                     latency for hash tables / order book lookups)
  ///   MADV_WILLNEED   — prefetch pages into page cache
  ///   MADV_DONTNEED   — release pages immediately (zeroed on next access;
  ///                     useful for pool reset without munmap/mmap cycle)
  bool advise(int advice) noexcept {
    if (addr_ == MAP_FAILED) {
      return false;
    }
    return ::madvise(addr_, size_, advice) == 0;
  }

  /// Lock pages into RAM (prevent swapout). May fail due to RLIMIT_MEMLOCK.
  bool lock() noexcept {
    if (addr_ == MAP_FAILED) {
      return false;
    }
    return ::mlock(addr_, size_) == 0;
  }

  /// Unlock pages (reverse of lock).
  bool unlock() noexcept {
    if (addr_ == MAP_FAILED) {
      return false;
    }
    return ::munlock(addr_, size_) == 0;
  }

  /// Change memory protection flags.
  /// Common values:
  ///   PROT_NONE                — guard page (access causes SIGSEGV)
  ///   PROT_READ                — read-only
  ///   PROT_READ | PROT_WRITE   — read-write (default for most factories)
  ///
  /// @warning Never use PROT_EXEC unless implementing a JIT compiler.
  ///   Writable + executable memory (W^X violation) allows injected data
  ///   to be executed as code — a classic exploitation primitive.
  ///   All factories use PROT_READ | PROT_WRITE without PROT_EXEC.
  ///
  /// Use cases:
  ///   - Guard pages at buffer boundaries for overflow detection.
  ///   - Make pool memory read-only after initialization (defense in depth).
  ///   - Temporarily mark shared memory read-only for consumer processes.
  bool protect(int prot) noexcept {
    if (addr_ == MAP_FAILED) {
      return false;
    }
    return ::mprotect(addr_, size_, prot) == 0;
  }

  /// Resize the mapping via mremap(2). May move the mapping if necessary
  /// (MREMAP_MAYMOVE). After a successful remap, get() may return a
  /// different address than before — invalidating any prior pointers.
  ///
  /// Useful for arena allocators that need to grow without re-allocating.
  /// Not suitable for ObjectPool (fixed-size by design).
  ///
  /// @param new_size New size in bytes. Must be > 0.
  /// @return true on success (addr_ and size_ updated), false on failure.
  [[nodiscard]] bool remap(std::size_t new_size) noexcept;

  /// Bind this region to a specific NUMA node via mbind(MPOL_BIND).
  /// Ensures all pages are allocated from the specified node's free list.
  /// Call after mmap but before first touch for best results (first-touch
  /// policy means the kernel allocates from the local node on first fault).
  ///
  /// @param node NUMA node ID (0-based). Check /sys/devices/system/node/
  ///             or lscpu for available nodes.
  /// @return true on success, false on failure (non-NUMA system, invalid node,
  ///         or insufficient memory on the requested node).
  ///
  /// @note On single-socket systems, this is a no-op — all memory is local.
  ///   On dual-socket HFT servers, binding the pool to the NIC-local node
  ///   avoids cross-socket QPI/UPI traffic on every packet.
  bool bind_numa_node(int node) noexcept;

  // ===========================================================================
  // Factory Methods (cold path — return std::optional)
  // ===========================================================================
  //
  // Different scenarios require fundamentally different syscall sequences.
  // Named factories document intent and prevent flag-composition mistakes.
  // The abort policy belongs in the caller, not here — so we return
  // std::optional and let callers like ObjectPool abort if needed.
  //
  // errno preservation: all factory error paths save errno before signal_log
  // and restore it afterward, so callers can inspect errno after nullopt.

  /// Anonymous private mapping. Size rounded up to kPageSize4K.
  /// Use for general-purpose pre-allocated buffers.
  ///
  /// @param lock_pages If true, uses MAP_LOCKED to pin pages at creation time.
  ///   Eliminates the window between mmap and mlock where pages could be
  ///   swapped out. May fail due to RLIMIT_MEMLOCK.
  [[nodiscard]] static std::optional<MmapRegion>
  allocate_anonymous(std::size_t size,
                     PrefaultPolicy pf = PrefaultPolicy::kNone,
                     bool lock_pages = false) noexcept;

  /// Explicit huge page mapping (MAP_HUGETLB).
  /// Size rounded up to the chosen huge page boundary.
  /// Returns nullopt if huge pages are not configured on the system.
  ///
  /// @note No automatic fallback to madvise(MADV_HUGEPAGE). The caller
  /// decides the fallback strategy explicitly (e.g., try huge pages first,
  /// then fall back to allocate_anonymous + advise(MADV_HUGEPAGE)).
  [[nodiscard]] static std::optional<MmapRegion>
  allocate_huge_pages(std::size_t size,
                      HugePageSize hp_size = HugePageSize::k2MB,
                      PrefaultPolicy pf = PrefaultPolicy::kNone,
                      bool lock_pages = false) noexcept;

  /// POSIX shared memory: shm_open + ftruncate + mmap(MAP_SHARED).
  ///
  /// @param name Must start with '/' and contain no other '/' (POSIX rule).
  /// @param size Region size in bytes. Rounded up to kPageSize4K.
  /// @param mode kCreateOrOpen (producer) or kOpenExisting (consumer).
  /// @param pf   Prefault policy (applied after mapping).
  /// @param lock_pages If true, uses MAP_LOCKED to pin pages at creation time.
  ///
  /// @note The fd is closed immediately after mmap — standard POSIX.
  ///   The kernel reference-counts the underlying shm object:
  ///     shm_open  → refcount 1 (fd)
  ///     mmap      → refcount 2 (fd + mapping)
  ///     close(fd) → refcount 1 (mapping alone keeps it alive)
  ///     munmap    → refcount 0 (kernel may reclaim)
  ///   Closing the fd early frees a scarce resource (default ulimit is
  ///   often 1024) while the mapping remains valid until munmap.
  ///   The shm object persists until shm_unlink() is called by the caller.
  ///   Cleanup (shm_unlink) is NOT MmapRegion's responsibility.
  [[nodiscard]] static std::optional<MmapRegion>
  open_shared(std::string_view name, std::size_t size,
              ShmMode mode = ShmMode::kCreateOrOpen,
              PrefaultPolicy pf = PrefaultPolicy::kNone,
              bool lock_pages = false) noexcept;

  /// File-backed mapping over an already-open fd.
  /// Maps a file directly into virtual address space — the kernel's page cache
  /// serves as the buffer, eliminating the read() → userspace copy step.
  /// MmapRegion does NOT own the fd — caller manages its lifetime.
  ///
  /// HFT use cases:
  ///   - Log replay / backtesting: mmap a multi-GB capture file and iterate
  ///     with pointer arithmetic — no read() syscalls, no buffer management.
  ///   - Configuration / symbol tables: mmap read-only at startup, OS handles
  ///     paging. Only touched pages consume physical memory.
  ///
  /// @param fd       Open file descriptor (must remain valid until mmap
  /// returns).
  /// @param size     Number of bytes to map.
  /// @param offset   Byte offset into the file (must be page-aligned).
  /// @param writable If true, MAP_SHARED + PROT_WRITE (changes persist to
  /// file).
  ///                 If false, MAP_PRIVATE + PROT_READ (copy-on-write, safer).
  /// @param lock_pages If true, uses MAP_LOCKED to pin pages at creation time.
  ///
  /// @note Validates that offset + size does not exceed file size. Mapping
  ///   beyond EOF can succeed on Linux but causes SIGBUS on access.
  [[nodiscard]] static std::optional<MmapRegion>
  map_file(int fd, std::size_t size, std::size_t offset = 0,
           bool writable = false, bool lock_pages = false) noexcept;

  /// File-backed mapping with automatic size detection via fstat.
  /// Maps the entire file from the beginning.
  ///
  /// @param fd       Open file descriptor.
  /// @param writable If true, MAP_SHARED + PROT_WRITE.
  /// @param lock_pages If true, uses MAP_LOCKED.
  [[nodiscard]] static std::optional<MmapRegion>
  map_file(int fd, bool writable = false, bool lock_pages = false) noexcept;

private:
  // ---------------------------------------------------------------------------
  // Private constructor — used only by factory methods.
  // ---------------------------------------------------------------------------
  MmapRegion(void *addr, std::size_t size) noexcept;

  // ---------------------------------------------------------------------------
  // Prefault helper — applies the requested policy after a successful mmap.
  // ---------------------------------------------------------------------------
  static bool apply_prefault(void *addr, std::size_t size,
                             PrefaultPolicy policy) noexcept;
};

} // namespace mk::sys::memory
