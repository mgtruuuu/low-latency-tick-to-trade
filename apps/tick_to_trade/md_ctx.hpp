/**
 * @file md_ctx.hpp
 * @brief MD feed thread recvmmsg() buffer context.
 *
 * Simple context struct holding pointers into a caller-provided contiguous
 * buffer.  No ownership, no logic — just typed views over the raw buffer
 * with the recvmmsg() 3-layer pointer chain (mmsghdr → iovec → data) wired
 * at construction time by make_md_ctx().
 *
 * Layout (single contiguous region):
 *   [mmsg_bufs: batch_size * buf_size]             — UDP recv datagram slots
 *   [iovecs:    batch_size * sizeof(iovec)]        — recvmmsg scatter-gather
 *   [msgvec:    batch_size * sizeof(mmsghdr)]      — recvmmsg message headers
 *   [cmsg_bufs: batch_size * kCmsgBufSize]         — SO_TIMESTAMPNS cmsg slots
 */

#pragma once

#include "sys/bit_utils.hpp"

#include <cstddef>
#include <cstring>
#include <ctime>
#include <sys/socket.h>

namespace mk::app {

/// Per-mmsghdr ancillary data buffer size for SO_TIMESTAMPNS cmsg.
/// Sized to hold one SCM_TIMESTAMPNS (struct timespec) cmsg with padding.
inline constexpr std::size_t kCmsgBufSize = CMSG_SPACE(sizeof(struct timespec));

/// Non-owning typed views into a contiguous recvmmsg() buffer region.
/// Caller is responsible for allocation, NUMA binding, and lifetime.
struct MdCtx {
  char *mmsg_bufs;
  struct iovec *iovecs;
  struct mmsghdr *msgvec;
  char *cmsg_bufs; // SO_TIMESTAMPNS ancillary data, batch_size * kCmsgBufSize
  unsigned int batch_size;
  std::size_t buf_size;
};

/// Compute minimum buffer size needed for a given batch/buf configuration.
[[nodiscard]] constexpr std::size_t
md_ctx_buf_size(unsigned int batch_size, std::size_t buf_size) noexcept {
  std::size_t offset = 0;

  offset += batch_size * buf_size;

  offset = sys::align_up(offset, alignof(struct iovec));
  offset += batch_size * sizeof(struct iovec);

  offset = sys::align_up(offset, alignof(struct mmsghdr));
  offset += batch_size * sizeof(struct mmsghdr);

  // cmsg buffers (one per mmsghdr) for SO_TIMESTAMPNS.
  offset = sys::align_up(offset, alignof(struct cmsghdr));
  offset += batch_size * kCmsgBufSize;

  return offset;
}

/// Carve a contiguous buffer into recvmmsg() scatter-gather arrays and wire
/// the 3-layer pointer chain (mmsghdr → iovec → data slots).
///
/// @param base        Pointer to buffer (>= md_ctx_buf_size bytes).
///                    Caller owns the buffer and must keep it alive.
/// @param batch_size  Max datagrams per recvmmsg() call.
/// @param buf_size    Per-datagram buffer capacity (bytes). Must be >= the
///                    largest UDP message type the exchange can send.
[[nodiscard]] inline MdCtx make_md_ctx(void *base, unsigned int batch_size,
                                       std::size_t buf_size) noexcept {
  auto *raw = static_cast<std::byte *>(base);
  std::size_t offset = 0;

  auto *bufs = reinterpret_cast<char *>(raw + offset);
  offset += batch_size * buf_size;

  offset = sys::align_up(offset, alignof(struct iovec));
  auto *iovs = reinterpret_cast<struct iovec *>(raw + offset);
  offset += batch_size * sizeof(struct iovec);

  offset = sys::align_up(offset, alignof(struct mmsghdr));
  auto *msgs = reinterpret_cast<struct mmsghdr *>(raw + offset);
  offset += batch_size * sizeof(struct mmsghdr);

  offset = sys::align_up(offset, alignof(struct cmsghdr));
  auto *cmsgs = reinterpret_cast<char *>(raw + offset);

  // Zero mmsghdr array before wiring linkage.
  std::memset(msgs, 0, batch_size * sizeof(struct mmsghdr));

  // Wire iovec/mmsghdr pointer chain + per-mmsghdr cmsg buffer (cold path,
  // done once). msg_controllen is reset before each recvmmsg() call in the
  // hot loop because the kernel overwrites it with the actual cmsg length.
  for (unsigned int j = 0; j < batch_size; ++j) {
    iovs[j].iov_base = bufs + (j * buf_size);
    iovs[j].iov_len = buf_size;
    msgs[j].msg_hdr.msg_iov = &iovs[j];
    msgs[j].msg_hdr.msg_iovlen = 1;
    msgs[j].msg_hdr.msg_control = cmsgs + (j * kCmsgBufSize);
    msgs[j].msg_hdr.msg_controllen = kCmsgBufSize;
  }

  return {
      .mmsg_bufs = bufs,
      .iovecs = iovs,
      .msgvec = msgs,
      .cmsg_bufs = cmsgs,
      .batch_size = batch_size,
      .buf_size = buf_size,
  };
}

} // namespace mk::app
