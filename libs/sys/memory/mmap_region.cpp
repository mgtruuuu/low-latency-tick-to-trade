/**
 * @file mmap_region.cpp
 * @brief Implementation of MmapRegion factory methods and syscall-heavy
 * operations.
 *
 * Separated from mmap_region.hpp to reduce recompilation for includers.
 * Only non-trivial, cold-path operations live here — inline observers
 * and one-liner wrappers remain in the header.
 */

#include "sys/memory/mmap_region.hpp"

#include "sys/bit_utils.hpp"
#include "sys/hardware_constants.hpp"
#include "sys/log/signal_logger.hpp"

#include <cerrno>
#include <cstring> // std::memcpy
#include <limits>  // std::numeric_limits

#include <fcntl.h>       // O_CREAT, O_RDWR, O_EXCL, shm_open
#include <sys/stat.h>    // fstat, struct stat
#include <sys/syscall.h> // SYS_mbind
#include <unistd.h>      // close, ftruncate

// MAP_HUGE_2MB / MAP_HUGE_1GB may not be defined on older kernels.
// The formula is: log2(pagesize) << MAP_HUGE_SHIFT.
#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif

#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif

#ifndef MAP_HUGE_1GB
#define MAP_HUGE_1GB (30 << MAP_HUGE_SHIFT)
#endif

// MADV_POPULATE_WRITE / MADV_POPULATE_READ: Linux 5.14+.
// Prefaults pages without a user-space loop.
#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif

#ifndef MADV_POPULATE_READ
#define MADV_POPULATE_READ 22
#endif

// NUMA memory policy constants (from <linux/mempolicy.h>).
// Defined here to avoid requiring numactl-devel headers.
// MPOL_BIND: Strictly bind memory allocation to the specified NUMA node(s).
// The kernel will only allocate pages from the specified node's free list.
#ifndef MPOL_BIND
#define MPOL_BIND 2
#endif

namespace {

[[nodiscard]] void *map_private_rw_or_log(std::size_t size, int flags,
                                          const char *caller,
                                          const char *hint = nullptr) noexcept {
  void *addr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (addr != MAP_FAILED) {
    return addr;
  }

  const int saved = errno;
  if (hint != nullptr) {
    mk::sys::log::signal_log(caller, ": mmap failed, size=", size, ", errno=",
                             saved, " ", hint, '\n');
  } else {
    mk::sys::log::signal_log(caller, ": mmap failed, size=", size,
                             ", errno=", saved, '\n');
  }
  errno = saved;
  return MAP_FAILED;
}

} // namespace

namespace mk::sys::memory {

// =============================================================================
// Private constructor
// =============================================================================

MmapRegion::MmapRegion(void *addr, std::size_t size) noexcept
    : addr_(addr), size_(size) {}

// =============================================================================
// Memory Operations (cold path)
// =============================================================================

bool MmapRegion::prefault(bool write_touch) noexcept {
  if (addr_ == MAP_FAILED) {
    return false;
  }

  // Delegate to apply_prefault which tries the efficient madvise path
  // (MADV_POPULATE_WRITE/READ, Linux 5.14+) before falling back to
  // a manual per-page touch loop.
  const auto policy = write_touch ? PrefaultPolicy::kPopulateWrite
                                  : PrefaultPolicy::kPopulateRead;
  return apply_prefault(addr_, size_, policy);
}

bool MmapRegion::remap(std::size_t new_size) noexcept {
  if (addr_ == MAP_FAILED || new_size == 0) {
    log::signal_log("MmapRegion::remap: invalid state (valid=",
                    (addr_ != MAP_FAILED) ? 1 : 0, ", new_size=", new_size,
                    ")\n");
    return false;
  }

  void *new_addr = ::mremap(addr_, size_, new_size, MREMAP_MAYMOVE);
  if (new_addr == MAP_FAILED) {
    const int saved = errno;
    log::signal_log("MmapRegion::remap: mremap failed, old_size=", size_,
                    ", new_size=", new_size, ", errno=", saved, '\n');
    errno = saved;
    return false;
  }

  addr_ = new_addr;
  size_ = new_size;
  return true;
}

bool MmapRegion::bind_numa_node(int node) noexcept {
  if (addr_ == MAP_FAILED || node < 0 || node >= 64) {
    return false;
  }

#ifdef SYS_mbind
  // Build a single-node bitmask. mbind expects an array of unsigned long
  // where bit N means NUMA node N.
  //
  // [Known limitation] unsigned long = 64 bits → max 64 NUMA nodes.
  // HPC systems with 64+ nodes would need a dynamically-sized bitmask
  // (array of unsigned long). HFT servers are 1–2 sockets (node 0–1),
  // so a single unsigned long is the fastest and simplest choice.
  unsigned long nodemask = 1UL << static_cast<unsigned>(node);
  // maxnode: the kernel reads this many bits from nodemask.
  // sizeof(unsigned long) * 8 = 64, covering all bits in our mask.
  constexpr unsigned long kMaxNode = sizeof(unsigned long) * 8;

  // Last argument is flags (bitwise OR):
  //   0              — apply policy to new allocations only (cheapest).
  //   MPOL_MF_MOVE   — also migrate already-faulted pages to the target node.
  //                    Useful if pages were faulted before bind_numa_node().
  //   MPOL_MF_STRICT — fail (EINVAL) if any existing page cannot be moved.
  //                    Without this, unmovable pages are silently skipped.
  // We use 0 because bind_numa_node() is meant to be called before first
  // touch — no pages to migrate yet.
  const long ret =
      ::syscall(SYS_mbind, addr_, size_, MPOL_BIND, &nodemask, kMaxNode, 0U);
  if (ret != 0) {
    const int saved = errno;
    log::signal_log("MmapRegion::bind_numa_node: mbind failed, node=", node,
                    ", errno=", saved, '\n');
    errno = saved;
    return false;
  }
  return true;
#else
  // Non-Linux or missing mbind syscall.
  (void)node;
  return false;
#endif
}

// =============================================================================
// Factory Methods
// =============================================================================

std::optional<MmapRegion>
MmapRegion::allocate_anonymous(std::size_t size, PrefaultPolicy pf,
                               bool lock_pages) noexcept {
  if (size == 0) {
    log::signal_log("MmapRegion::allocate_anonymous: size is 0\n");
    return std::nullopt;
  }

  const std::size_t rounded = align_up(size, kPageSize4K);

  // MAP_PRIVATE:    copy-on-write — writes are process-private, not
  //                 shared with child processes or backed by a file.
  // MAP_ANONYMOUS:  no file backing — pages are zero-initialized.
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  // MAP_POPULATE: pre-populate page tables at mmap time (Linux 2.5.46+).
  // Best-effort hint, not a hard residency guarantee.
  // Complements apply_prefault() below which deterministically faults pages.
  if (pf != PrefaultPolicy::kNone) {
    flags |= MAP_POPULATE;
  }
  // MAP_LOCKED: pin pages in RAM from creation — no window between mmap
  // and mlock where pages could be swapped out. Fails with EPERM if
  // RLIMIT_MEMLOCK is exceeded (modern Linux 2.6.9+).
  // Note: population may silently fail (pages locked but not yet resident);
  // apply_prefault() below guarantees all pages are faulted in.
  if (lock_pages) {
    flags |= MAP_LOCKED;
  }

  void *addr =
      map_private_rw_or_log(rounded, flags, "MmapRegion::allocate_anonymous");
  if (addr == MAP_FAILED) {
    return std::nullopt;
  }

  MmapRegion region(addr, rounded);
  if (!apply_prefault(addr, rounded, pf)) {
    return std::nullopt; // region destructor will munmap.
  }
  return region;
}

std::optional<MmapRegion>
MmapRegion::allocate_huge_pages(std::size_t size, HugePageSize hp_size,
                                PrefaultPolicy pf, bool lock_pages) noexcept {
  if (size == 0) {
    log::signal_log("MmapRegion::allocate_huge_pages: size is 0\n");
    return std::nullopt;
  }

  std::size_t page_size = 0;
  int huge_flag = 0;

  switch (hp_size) {
  case HugePageSize::k2MB:
    page_size = kHugePageSize2MB;
    huge_flag = MAP_HUGE_2MB;
    break;
  case HugePageSize::k1GB:
    page_size = kHugePageSize1GB;
    huge_flag = MAP_HUGE_1GB;
    break;
  default:
    log::signal_log(
        "MmapRegion::allocate_huge_pages: unsupported HugePageSize\n");
    return std::nullopt;
  }

  const std::size_t rounded = align_up(size, page_size);

  // MAP_PRIVATE:    copy-on-write (process-private pages).
  // MAP_ANONYMOUS:  no file backing.
  // MAP_HUGETLB:    allocate from the hugetlb pool (/proc/sys/vm/nr_hugepages).
  //                 Pages are inherently non-swappable (pinned in RAM).
  // huge_flag:      MAP_HUGE_2MB or MAP_HUGE_1GB — selects page granularity.
  int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | huge_flag;
  // MAP_POPULATE here is also best-effort; apply_prefault() below is the
  // deterministic step for first-touch behavior.
  if (pf != PrefaultPolicy::kNone) {
    flags |= MAP_POPULATE;
  }
  // MAP_LOCKED is mostly redundant for MAP_HUGETLB mappings:
  // hugetlb pages are not swappable, and MAP_HUGETLB does not internally
  // fall back to 4KB pages (mmap fails if huge-page allocation cannot be met).
  // We keep the flag for policy symmetry with non-hugetlb paths.
  if (lock_pages) {
    flags |= MAP_LOCKED;
  }

  void *addr = map_private_rw_or_log(
      rounded, flags, "MmapRegion::allocate_huge_pages",
      "(check /proc/sys/vm/nr_hugepages or "
      "/sys/kernel/mm/hugepages/hugepages-*/nr_hugepages)");
  if (addr == MAP_FAILED) {
    return std::nullopt;
  }

  MmapRegion region(addr, rounded);
  if (!apply_prefault(addr, rounded, pf)) {
    return std::nullopt; // region destructor will munmap.
  }
  return region;
}

std::optional<MmapRegion>
MmapRegion::open_shared(std::string_view name, std::size_t size, ShmMode mode,
                        PrefaultPolicy pf, bool lock_pages) noexcept {
  if (size == 0 || name.empty()) {
    log::signal_log("MmapRegion::open_shared: invalid args (size=", size,
                    ", name empty=", name.empty() ? 1 : 0, ")\n");
    return std::nullopt;
  }

  // shm_open requires a null-terminated C string.
  // string_view is not null-terminated, so copy to a stack buffer.
  static constexpr std::size_t kMaxNameLen = 255;
  if (name.size() >= kMaxNameLen) {
    log::signal_log("MmapRegion::open_shared: name too long (max 254 chars)\n");
    return std::nullopt;
  }

  char name_buf[kMaxNameLen + 1];
  std::memcpy(name_buf, name.data(), name.size());
  name_buf[name.size()] = '\0';

  // Determine flags and open the shm object.
  const std::size_t rounded = align_up(size, kPageSize4K);

  // Guard against size_t → off_t overflow (signed 64-bit max ~9.2 EB).
  // Checked here (precondition) rather than deep in the ftruncate path.
  if (rounded > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
    log::signal_log("MmapRegion::open_shared: size exceeds off_t range\n");
    return std::nullopt;
  }

  // fd is a process-local index into the per-process file descriptor table.
  // Two processes opening the same shm object get different fd values
  // (e.g., A gets fd=3, B gets fd=5), but both point to the same kernel
  // inode. Even within one process, two opens yield distinct fds.
  int fd = -1;
  bool created = false;

  if (mode == ShmMode::kCreateOrOpen) {
    // Try O_CREAT | O_EXCL first — atomic create-or-fail.
    // This avoids the race where two processes both create and ftruncate
    // concurrently. Only the winner creates; the loser falls through
    // to open-existing and validates the size via fstat.
    fd = ::shm_open(name_buf, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      created = true;
    } else if (errno == EEXIST) {
      // Object already exists — open without O_EXCL.
      fd = ::shm_open(name_buf, O_RDWR, 0600);
    }
  } else {
    // kOpenExisting: open only, fail if absent.
    fd = ::shm_open(name_buf, O_RDWR, 0600);
  }

  if (fd < 0) {
    const int saved = errno;
    log::signal_log("MmapRegion::open_shared: shm_open failed, name=", name_buf,
                    ", errno=", saved, '\n');
    errno = saved;
    return std::nullopt;
  }

  // ftruncate sets the shm object's logical size (metadata only).
  // Physical pages are NOT allocated here — they are allocated lazily
  // on first access (page fault) or eagerly via MAP_POPULATE / prefault.
  //
  // Only ftruncate when we just created the object. Re-opening an
  // existing object validates size via fstat instead of blindly resizing,
  // which could corrupt data or cause SIGBUS in other processes with
  // existing mappings.
  if (created) {
    if (::ftruncate(fd, static_cast<off_t>(rounded)) != 0) {
      const int saved = errno;
      log::signal_log("MmapRegion::open_shared: ftruncate failed, size=",
                      rounded, ", errno=", saved, '\n');
      errno = saved;
      ::close(fd);
      // Clean up the newly created shm object to avoid leaving a stale
      // zero-sized object in /dev/shm that would confuse future opens.
      ::shm_unlink(name_buf);
      return std::nullopt;
    }
  } else {
    // Validate the caller's requested size against the actual shm object
    // size. Mapping beyond the object's bounds causes SIGBUS on access
    // — fail early instead.
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
      const int saved = errno;
      log::signal_log("MmapRegion::open_shared: fstat failed, errno=", saved,
                      '\n');
      errno = saved;
      ::close(fd);
      return std::nullopt;
    }
    const auto actual_size = static_cast<std::size_t>(st.st_size);
    if (rounded > actual_size) {
      log::signal_log("MmapRegion::open_shared: requested size ", rounded,
                      " exceeds shm object size ", actual_size, '\n');
      ::close(fd);
      return std::nullopt;
    }
  }

  // MAP_SHARED: writes visible to all processes mapping this shm object.
  // Required for IPC — MAP_PRIVATE would create a process-private copy.
  int mmap_flags = MAP_SHARED;
  if (lock_pages) {
    mmap_flags |= MAP_LOCKED;
  }

  // mmap maps the shm object into this process's virtual address space.
  // Each process gets a different virtual address, but page tables point
  // to the same physical frames. Only PTEs are set up here — physical
  // allocation happens on first touch or via prefault.
  void *addr =
      ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE, mmap_flags, fd, 0);

  // Close fd immediately. The kernel maintains a reference to the
  // underlying file object for the mapping, so it remains valid until
  // munmap. See the @note on open_shared() in the header for details.
  ::close(fd);

  if (addr == MAP_FAILED) {
    const int saved = errno;
    log::signal_log("MmapRegion::open_shared: mmap failed, size=", rounded,
                    ", errno=", saved, '\n');
    errno = saved;
    if (created) {
      ::shm_unlink(name_buf);
    }
    return std::nullopt;
  }

  MmapRegion region(addr, rounded);
  if (!apply_prefault(addr, rounded, pf)) {
    if (created) {
      ::shm_unlink(name_buf);
    }
    return std::nullopt; // region destructor will munmap.
  }
  return region;
}

std::optional<MmapRegion> MmapRegion::map_file(int fd, std::size_t size,
                                               std::size_t offset,
                                               bool writable,
                                               bool lock_pages) noexcept {
  if (size == 0 || fd < 0) {
    log::signal_log("MmapRegion::map_file: invalid args (size=", size,
                    ", fd=", fd, ")\n");
    return std::nullopt;
  }

  // mmap requires offset to be a multiple of the page size.
  // Catch this early with a clear failure instead of a cryptic EINVAL.
  if (offset % kPageSize4K != 0) {
    log::signal_log("MmapRegion::map_file: offset ", offset,
                    " is not page-aligned\n");
    return std::nullopt;
  }

  // Guard against size_t → off_t overflow (signed 64-bit max ~9.2 EB).
  if (offset > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
    log::signal_log("MmapRegion::map_file: offset exceeds off_t range\n");
    return std::nullopt;
  }

  // Validate that the requested range fits within the file.
  // Mapping beyond EOF succeeds on Linux but causes SIGBUS on access.
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    const int saved = errno;
    log::signal_log("MmapRegion::map_file: fstat failed, errno=", saved, '\n');
    errno = saved;
    return std::nullopt;
  }
  const auto file_size = static_cast<std::size_t>(st.st_size);
  // Use subtraction-based guard to avoid size_t overflow.
  // If offset + size wraps around, the naive check passes incorrectly,
  // leading to mmap beyond EOF and SIGBUS on access.
  if (offset > file_size || size > file_size - offset) {
    log::signal_log("MmapRegion::map_file: offset=", offset, " + size=", size,
                    " exceeds file size=", file_size, '\n');
    return std::nullopt;
  }

  int prot = PROT_READ;
  // MAP_PRIVATE: copy-on-write — reads see file contents, writes are
  // process-private (file is not modified). Safer default for read-only.
  int flags = MAP_PRIVATE;

  if (writable) {
    prot |= PROT_WRITE;
    // MAP_SHARED: writes propagate to the underlying file (and are
    // visible to other processes mapping the same file). Required for
    // shared-memory IPC and persistent file modification.
    flags = MAP_SHARED;
  }
  if (lock_pages) {
    flags |= MAP_LOCKED;
  }

  void *addr =
      ::mmap(nullptr, size, prot, flags, fd, static_cast<off_t>(offset));

  if (addr == MAP_FAILED) {
    const int saved = errno;
    log::signal_log("MmapRegion::map_file: mmap failed, size=", size,
                    ", offset=", offset, ", errno=", saved, '\n');
    errno = saved;
    return std::nullopt;
  }

  return MmapRegion(addr, size);
}

std::optional<MmapRegion> MmapRegion::map_file(int fd, bool writable,
                                               bool lock_pages) noexcept {
  if (fd < 0) {
    return std::nullopt;
  }

  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    const int saved = errno;
    log::signal_log("MmapRegion::map_file: fstat failed, errno=", saved, '\n');
    errno = saved;
    return std::nullopt;
  }

  // Only regular files have a meaningful st_size for mmap.
  // Pipes, sockets, and device files report 0 or garbage — reject them.
  if (!S_ISREG(st.st_mode)) {
    log::signal_log("MmapRegion::map_file: fd is not a regular file\n");
    return std::nullopt;
  }

  if (st.st_size <= 0) {
    log::signal_log("MmapRegion::map_file: file is empty\n");
    return std::nullopt;
  }

  return map_file(fd, static_cast<std::size_t>(st.st_size), 0, writable,
                  lock_pages);
}

// =============================================================================
// Prefault helper (private static)
// =============================================================================

bool MmapRegion::apply_prefault(void *addr, std::size_t size,
                                PrefaultPolicy policy) noexcept {
  switch (policy) {
  case PrefaultPolicy::kNone:
    return true;

  case PrefaultPolicy::kPopulateWrite: {
    // MADV_POPULATE_WRITE (Linux 5.14+): kernel prefaults all pages.
    // Much faster than a user-space loop — no page fault per page.
    const int ret = ::madvise(addr, size, MADV_POPULATE_WRITE);
    if (ret == 0) {
      return true;
    }
    // EINVAL means the kernel doesn't recognize MADV_POPULATE_WRITE
    // (pre-5.14). Fall through to manual write-touch for portability.
    // Any other error (ENOMEM, EFAULT, etc.) is a real failure — report it.
    if (errno != EINVAL) {
      const int saved = errno;
      log::signal_log("MmapRegion::apply_prefault: madvise "
                      "MADV_POPULATE_WRITE failed, errno=",
                      saved, '\n');
      errno = saved;
      return false;
    }
    [[fallthrough]];
  }

  case PrefaultPolicy::kManualWrite: {
    // Write-touch each page. Forces page faults during cold-path setup
    // so the hot path never takes a first-touch fault.
    //
    // [Known tradeoff] Stride is always 4KB, even for huge pages.
    // With 2MB huge pages: 512 writes per page (2MB/4KB), but only
    // the first write triggers the actual page fault — the rest are
    // plain memory stores (no TLB miss, no kernel entry).
    // With 1GB huge pages: 262,144 writes, same reasoning.
    // Using page_size as stride would be faster, but apply_prefault
    // doesn't know the page size. This is cold path, so the extra
    // writes (~1ms for 1GB) are acceptable vs. adding complexity.
    // For hot-path-critical prefault, use kPopulateWrite instead —
    // the kernel handles it optimally regardless of page size.
    auto *base = static_cast<volatile std::byte *>(addr);
    const std::size_t stride = kPageSize4K;
    for (std::size_t off = 0; off < size; off += stride) {
      base[off] = std::byte{0};
    }
    return true;
  }

  case PrefaultPolicy::kPopulateRead: {
    // MADV_POPULATE_READ (Linux 5.14+): kernel prefaults pages read-only.
    // Safe for shared memory consumers — populates page tables without
    // modifying the underlying data.
    const int ret = ::madvise(addr, size, MADV_POPULATE_READ);
    if (ret == 0) {
      return true;
    }
    if (errno != EINVAL) {
      const int saved = errno;
      log::signal_log("MmapRegion::apply_prefault: madvise "
                      "MADV_POPULATE_READ failed, errno=",
                      saved, '\n');
      errno = saved;
      return false;
    }
    [[fallthrough]];
  }

  case PrefaultPolicy::kManualRead: {
    // Read-touch each page. Populates page tables without dirtying pages.
    // Safe for shared memory consumers — preserves existing data.
    const auto *base = static_cast<const volatile std::byte *>(addr);
    const std::size_t stride = kPageSize4K;
    for (std::size_t off = 0; off < size; off += stride) {
      [[maybe_unused]] const volatile std::byte dummy = base[off];
    }
    return true;
  }
  }
  return true; // Unreachable, but silences compiler warnings.
}

} // namespace mk::sys::memory
