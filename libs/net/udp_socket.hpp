/**
 * @file udp_socket.hpp
 * @brief RAII UDP datagram socket wrapper with sendto/recvfrom utilities.
 *
 * Inherits SocketBase (abort-on-invalid, observers, socket options).
 * Adds UDP-specific operations: sendto, recvfrom with sockaddr_in
 * addressing, and multicast group management.
 *
 * Design hierarchy:
 *   ScopedFd           — fd + close()
 *     +-- SocketBase   — ScopedFd + abort-on-invalid + observers + socket
 * options |     +-- TcpSocket  — SocketBase + TCP send/recv + TCP options | +--
 * UdpSocket  — SocketBase + UDP sendto/recvfrom + multicast (this file)
 *     +-- EpollWrapper — ScopedFd + epoll_ctl/epoll_wait
 *
 * Key differences from TCP (TcpSocket):
 *   - Send is atomic: one sendto() = one datagram. No partial-write loops.
 *   - Recv preserves message boundaries: one recvfrom() = one datagram.
 *   - recvfrom() == 0 means an empty datagram (valid!), NOT peer close.
 *   - No kPeerClosed status — UDP is connectionless.
 *   - Every sendto/recvfrom carries an explicit sockaddr_in address.
 */

#pragma once

#include "net/socket_base.hpp"

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <netinet/in.h>
#include <sys/socket.h>

namespace mk::net {

class UdpSocket : public SocketBase {
public:
  /**
   * @brief Constructs a UdpSocket from a raw file descriptor.
   * @param fd The raw file descriptor to wrap (must be a valid SOCK_DGRAM fd).
   */
  explicit UdpSocket(int fd) noexcept : SocketBase(fd) {}

  // Move support — delegates to SocketBase's move operations.
  UdpSocket(UdpSocket &&) noexcept = default;
  UdpSocket &operator=(UdpSocket &&) noexcept = default;

  // Non-copyable (unique fd ownership).
  UdpSocket(const UdpSocket &) = delete;
  UdpSocket &operator=(const UdpSocket &) = delete;

  // SocketBase (via ScopedFd) handles close() — destructor is defaulted.
  ~UdpSocket() = default;

  // -- Multicast --
  //
  // HFT market data feeds use UDP multicast for one-to-many distribution.
  // The kernel's IGMP stack handles group membership; these methods configure
  // the socket-level options that control join/leave and send behavior.

  /**
   * @brief Joins a multicast group (IP_ADD_MEMBERSHIP).
   *
   * The kernel sends an IGMP Join message to the network. Incoming datagrams
   * addressed to `group_addr` on `interface_addr` will be delivered to this
   * socket. The socket must be bound (typically to INADDR_ANY + the group port)
   * before joining.
   *
   * @param group_addr     Multicast group (e.g., inet_addr("239.255.0.1")).
   * @param interface_addr Local NIC to join on (INADDR_ANY = kernel picks).
   * @return true on success.
   */
  [[nodiscard]] bool join_multicast_group(in_addr group_addr,
                                          in_addr interface_addr) noexcept {
    ip_mreq mreq{};
    mreq.imr_multiaddr = group_addr;
    mreq.imr_interface = interface_addr;
    return setsockopt(fd_.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                      sizeof(mreq)) == 0;
  }

  /// Convenience overload: join by dotted-decimal strings.
  /// @param group  Null-terminated multicast address (e.g., "239.255.0.1").
  /// @param iface  Null-terminated interface IP (e.g., "10.0.0.1"), or nullptr
  ///               for INADDR_ANY. Both must be null-terminated — inet_pton
  ///               requires C strings.
  /// @return true on success, false on invalid address or setsockopt failure.
  [[nodiscard]] bool join_multicast_group(const char *group,
                                          const char *iface = nullptr) noexcept {
    in_addr group_addr{};
    if (inet_pton(AF_INET, group, &group_addr) != 1) {
      return false;
    }
    in_addr iface_addr{};
    if (iface != nullptr) {
      if (inet_pton(AF_INET, iface, &iface_addr) != 1) {
        return false;
      }
    } else {
      iface_addr.s_addr = INADDR_ANY;
    }
    return join_multicast_group(group_addr, iface_addr);
  }

  /**
   * @brief Leaves a multicast group (IP_DROP_MEMBERSHIP).
   *
   * Sends IGMP Leave. After this call, the kernel stops delivering datagrams
   * for this group to this socket. Safe to call even if not currently joined
   * (returns false, but no harm done).
   *
   * @param group_addr     Multicast group to leave.
   * @param interface_addr Local NIC (must match the join call).
   * @return true on success.
   */
  [[nodiscard]] bool leave_multicast_group(in_addr group_addr,
                                           in_addr interface_addr) noexcept {
    ip_mreq mreq{};
    mreq.imr_multiaddr = group_addr;
    mreq.imr_interface = interface_addr;
    return setsockopt(fd_.get(), IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq,
                      sizeof(mreq)) == 0;
  }

  /**
   * @brief Sets the multicast TTL (IP_MULTICAST_TTL).
   *
   * Controls how many router hops a multicast datagram can traverse.
   *   TTL 0 — restricted to same host (loopback testing).
   *   TTL 1 — restricted to same LAN (HFT standard — co-located switches).
   *   TTL >1 — crosses routers (rarely used in HFT).
   *
   * @param ttl Hop limit (0..255).
   * @return true on success.
   */
  [[nodiscard]] bool set_multicast_ttl(int ttl) noexcept {
    return setsockopt(fd_.get(), IPPROTO_IP, IP_MULTICAST_TTL, &ttl,
                      sizeof(ttl)) == 0;
  }

  /**
   * @brief Enables/disables multicast loopback (IP_MULTICAST_LOOP).
   *
   * When enabled (default), datagrams sent to a multicast group are also
   * looped back to sockets on the same host that have joined the group.
   * Essential for local testing; typically disabled in production to avoid
   * processing own messages.
   *
   * @param enable true = loopback on, false = loopback off.
   * @return true on success.
   */
  [[nodiscard]] bool set_multicast_loop(bool enable) noexcept {
    int val = enable ? 1 : 0;
    return setsockopt(fd_.get(), IPPROTO_IP, IP_MULTICAST_LOOP, &val,
                      sizeof(val)) == 0;
  }

  /**
   * @brief Sets the outgoing multicast interface (IP_MULTICAST_IF).
   *
   * Determines which NIC the kernel uses to send multicast datagrams.
   * In HFT, this pins multicast traffic to the dedicated market data NIC
   * (separate from order entry / management traffic).
   *
   * @param interface_addr Local NIC address (INADDR_ANY = kernel default).
   * @return true on success.
   */
  [[nodiscard]] bool set_multicast_interface(in_addr interface_addr) noexcept {
    return setsockopt(fd_.get(), IPPROTO_IP, IP_MULTICAST_IF, &interface_addr,
                      sizeof(interface_addr)) == 0;
  }

  // -- Sendto --

  /**
   * @brief Status codes for sendto operations.
   *
   * No kPeerClosed — UDP is connectionless, so EPIPE/ECONNRESET don't apply.
   *   kOk         — Datagram sent (bytes_sent == len, UDP is atomic).
   *   kWouldBlock — Non-blocking only: send buffer full.
   *   kError      — Fatal socket error (check err_no in SendtoResult).
   */
  enum class SendtoStatus { kOk, kWouldBlock, kError };

  /**
   * @brief Result of a sendto operation.
   *
   * bytes_sent is only meaningful when status == kOk.
   * For UDP, kOk always means bytes_sent == len (atomic send).
   */
  struct SendtoResult {
    SendtoStatus status = SendtoStatus::kError;
    ssize_t bytes_sent = 0;
    int err_no = 0;
  };

  /**
   * @brief Sends a datagram to the specified address, blocking until complete.
   *
   * UDP sends are atomic — the entire datagram is sent in one syscall or
   * the send fails entirely. No partial-write loop is needed (unlike TCP).
   *
   * MSG_NOSIGNAL: prevents SIGPIPE on connected UDP sockets.
   * EINTR retry: the do-while loop handles signal interruption.
   *
   * @param buf  Pointer to the datagram payload.
   * @param len  Length of the payload in bytes.
   * @param dest Destination address.
   * @pre buf != nullptr && len > 0
   * @return SendtoResult — bytes_sent == len when status == kOk.
   */
  [[nodiscard]] SendtoResult
  sendto_blocking(const char *buf, std::size_t len,
                  const struct sockaddr_in &dest) noexcept {
    assert(buf && len > 0 &&
           "sendto_blocking: buf must not be null and len must be > 0");
    ssize_t sent;
    do {
      sent = ::sendto(fd_.get(), buf, len, MSG_NOSIGNAL,
                      reinterpret_cast<const struct sockaddr *>(&dest),
                      sizeof(dest));
    } while (sent == -1 && errno == EINTR);

    if (sent == -1) {
      return {.status = SendtoStatus::kError, .bytes_sent = 0, .err_no = errno};
    }
    return {.status = SendtoStatus::kOk, .bytes_sent = sent, .err_no = 0};
  }

  /**
   * @brief Sends a datagram non-blocking (MSG_DONTWAIT).
   *
   * Returns kWouldBlock if the send buffer is full.
   *
   * @param buf  Pointer to the datagram payload.
   * @param len  Length of the payload in bytes.
   * @param dest Destination address.
   * @pre buf != nullptr && len > 0
   * @return SendtoResult — kOk, kWouldBlock, or kError.
   */
  [[nodiscard]] SendtoResult
  sendto_nonblocking(const char *buf, std::size_t len,
                     const struct sockaddr_in &dest) noexcept {
    assert(buf && len > 0 &&
           "sendto_nonblocking: buf must not be null and len must be > 0");
    ssize_t sent;
    do {
      sent = ::sendto(fd_.get(), buf, len, MSG_NOSIGNAL | MSG_DONTWAIT,
                      reinterpret_cast<const struct sockaddr *>(&dest),
                      sizeof(dest));
    } while (sent == -1 && errno == EINTR);

    if (sent == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return {.status = SendtoStatus::kWouldBlock, .bytes_sent = 0, .err_no = 0};
      }
      return {.status = SendtoStatus::kError, .bytes_sent = 0, .err_no = errno};
    }
    return {.status = SendtoStatus::kOk, .bytes_sent = sent, .err_no = 0};
  }

  // -- Recv (without source address) --

  /**
   * @brief Status codes for receive operations.
   *
   * Shared by recv and recvfrom variants.
   * No kPeerClosed — UDP recv() == 0 means an empty datagram, not FIN.
   *   kOk         — Datagram received (bytes_received >= 0).
   *   kWouldBlock — Non-blocking only: no datagram available.
   *   kError      — Fatal socket error (check err_no).
   */
  enum class RecvStatus { kOk, kWouldBlock, kError };

  /**
   * @brief Result of a recv operation (without source address).
   *
   * Lighter than RecvfromResult — no 16-byte sockaddr_in overhead.
   * Use when the sender's address is not needed (e.g., multicast receive
   * where all datagrams come from the same known source).
   */
  struct RecvResult {
    RecvStatus status = RecvStatus::kError;
    ssize_t bytes_received = 0;
    int err_no = 0;
  };

  /**
   * @brief Receives a single datagram non-blocking, without source address.
   *
   * Passes nullptr for src_addr/addrlen, avoiding the kernel's address
   * copy. For multicast hot paths where the source is irrelevant.
   *
   * @param buf Pointer to the receive buffer.
   * @param len Buffer capacity in bytes.
   * @pre buf != nullptr && len > 0
   * @return RecvResult — kOk, kWouldBlock, or kError.
   */
  [[nodiscard]] RecvResult recv_nonblocking(char *buf,
                                            std::size_t len) noexcept {
    assert(buf && len > 0 &&
           "recv_nonblocking: buf must not be null and len must be > 0");
    ssize_t n;
    do {
      n = ::recvfrom(fd_.get(), buf, len, MSG_DONTWAIT, nullptr, nullptr);
    } while (n == -1 && errno == EINTR);

    if (n == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return {.status = RecvStatus::kWouldBlock, .bytes_received = 0, .err_no = 0};
      }
      return {.status = RecvStatus::kError, .bytes_received = 0, .err_no = errno};
    }
    return {.status = RecvStatus::kOk, .bytes_received = n, .err_no = 0};
  }

  // -- Recvfrom (with source address) --

  // Alias for backward compatibility with recvfrom variants.
  using RecvfromStatus = RecvStatus;

  /**
   * @brief Result of a recvfrom operation.
   *
   * source_addr contains the sender's address (populated by the kernel).
   * bytes_received == 0 is valid — it means an empty datagram was received.
   */
  struct RecvfromResult {
    RecvfromStatus status = RecvfromStatus::kError;
    ssize_t bytes_received = 0;
    int err_no = 0;
    struct sockaddr_in source_addr {};
  };

  /**
   * @brief Receives a single datagram, blocking until one arrives.
   *
   * One recvfrom() = one datagram. Message boundaries are preserved.
   * The source address is stored in the result's source_addr field.
   *
   * @param buf Pointer to the receive buffer.
   * @param len Buffer capacity in bytes.
   * @pre buf != nullptr && len > 0
   * @return RecvfromResult — bytes_received >= 0 when status == kOk.
   */
  [[nodiscard]] RecvfromResult recvfrom_blocking(char *buf,
                                                 std::size_t len) noexcept {
    assert(buf && len > 0 &&
           "recvfrom_blocking: buf must not be null and len must be > 0");
    RecvfromResult result{};
    socklen_t addr_len = sizeof(result.source_addr);
    ssize_t n;
    do {
      n = ::recvfrom(fd_.get(), buf, len, 0,
                     reinterpret_cast<struct sockaddr *>(&result.source_addr),
                     &addr_len);
    } while (n == -1 && errno == EINTR);

    if (n == -1) {
      result.status = RecvfromStatus::kError;
      result.err_no = errno;
      return result;
    }
    // n == 0 is a valid empty datagram in UDP (NOT peer close like TCP).
    result.status = RecvfromStatus::kOk;
    result.bytes_received = n;
    return result;
  }

  /**
   * @brief Receives a single datagram non-blocking (MSG_DONTWAIT).
   *
   * Returns kWouldBlock if no datagram is available.
   *
   * @param buf Pointer to the receive buffer.
   * @param len Buffer capacity in bytes.
   * @pre buf != nullptr && len > 0
   * @return RecvfromResult — kOk, kWouldBlock, or kError.
   */
  [[nodiscard]] RecvfromResult recvfrom_nonblocking(char *buf,
                                                    std::size_t len) noexcept {
    assert(buf && len > 0 &&
           "recvfrom_nonblocking: buf must not be null and len must be > 0");
    RecvfromResult result{};
    socklen_t addr_len = sizeof(result.source_addr);
    ssize_t n;
    do {
      n = ::recvfrom(fd_.get(), buf, len, MSG_DONTWAIT,
                     reinterpret_cast<struct sockaddr *>(&result.source_addr),
                     &addr_len);
    } while (n == -1 && errno == EINTR);

    if (n == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        result.status = RecvfromStatus::kWouldBlock;
        return result;
      }
      result.status = RecvfromStatus::kError;
      result.err_no = errno;
      return result;
    }
    // n == 0 is a valid empty datagram in UDP (NOT peer close like TCP).
    result.status = RecvfromStatus::kOk;
    result.bytes_received = n;
    return result;
  }

  // -- Batch syscalls (recvmmsg / sendmmsg) --
  //
  // Linux batch syscalls for UDP datagrams. One syscall processes up to N
  // datagrams, reducing user↔kernel transition overhead.
  //
  // recvmmsg: High value in HFT. Market data arrives in bursts — multiple
  //   datagrams queue in the kernel receive buffer between epoll wakes.
  //   Draining them with one syscall instead of N recvfrom() calls saves
  //   ~200-400ns per burst on modern hardware.
  //
  // sendmmsg: Rarely useful in HFT. The sender controls timing, so there's
  //   no natural batching opportunity:
  //   - Order entry uses TCP (not UDP).
  //   - Market data publishing sends each tick immediately — batching would
  //     delay the first message until the batch is full, adding latency.
  //   Included for API completeness (e.g., market data gateway re-publishing
  //   to multiple multicast groups), not for hot-path trading use.
  //
  // Per-datagram metadata (byte count, source/dest address) lives inside
  // mmsghdr, not in the return value. The caller controls whether addresses
  // are populated by how they initialize msgvec[i].msg_hdr.msg_name:
  //   nullptr        — no address (multicast recv where source is irrelevant)
  //   &sockaddr_in   — recv: kernel fills source; send: specifies destination

  /**
   * @brief Result of a batch receive/send operation (recvmmsg / sendmmsg).
   *
   * count is the number of datagrams processed in one syscall.
   * For recvmmsg: 0 on EAGAIN (non-blocking) or error.
   * For sendmmsg: 0 on EAGAIN (non-blocking) or error.
   */
  struct MmsgResult {
    int count = 0;
    int err_no = 0;
  };

  /**
   * @brief Receives multiple datagrams in a single blocking syscall via
   *        recvmmsg(2).
   *
   * Blocks until at least one datagram is available. Each mmsghdr element
   * must be pre-initialized with iov buffer pointers. After the call,
   * msgvec[i].msg_len contains the byte count for datagram i (i < count).
   *
   * @param msgvec Pre-initialized array of mmsghdr structures.
   * @param vlen   Maximum number of datagrams to receive (array capacity).
   * @pre msgvec != nullptr && vlen > 0
   * @return MmsgResult — count of datagrams received, 0 on error.
   */
  [[nodiscard]] MmsgResult
  recvmmsg_blocking(struct mmsghdr *msgvec, unsigned int vlen) noexcept {
    assert(msgvec && vlen > 0 &&
           "recvmmsg_blocking: msgvec must not be null and vlen must be > 0");
    int n;
    do {
      n = ::recvmmsg(fd_.get(), msgvec, vlen, 0, nullptr);
    } while (n == -1 && errno == EINTR);

    if (n == -1) {
      return {.count = 0, .err_no = errno};
    }
    return {.count = n, .err_no = 0};
  }

  /**
   * @brief Receives multiple datagrams non-blocking via recvmmsg(2).
   *
   * Reduces syscall overhead when multiple datagrams are queued in the
   * socket receive buffer (common during market data bursts). Each mmsghdr
   * element must be pre-initialized with iov buffer pointers.
   *
   * After the call, each msgvec[i].msg_len contains the number of bytes
   * received for that datagram (for i < result.count).
   *
   * @param msgvec Pre-initialized array of mmsghdr structures.
   * @param vlen   Maximum number of datagrams to receive (array capacity).
   * @pre msgvec != nullptr && vlen > 0
   * @return MmsgResult — count of datagrams received, 0 on EAGAIN.
   */
  [[nodiscard]] MmsgResult
  recvmmsg_nonblocking(struct mmsghdr *msgvec, unsigned int vlen) noexcept {
    assert(msgvec && vlen > 0 &&
           "recvmmsg_nonblocking: msgvec must not be null and vlen must be > 0");
    int n;
    do {
      n = ::recvmmsg(fd_.get(), msgvec, vlen, MSG_DONTWAIT, nullptr);
    } while (n == -1 && errno == EINTR);

    if (n == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return {.count = 0, .err_no = 0};
      }
      return {.count = 0, .err_no = errno};
    }
    return {.count = n, .err_no = 0};
  }

  // -- Batch send (sendmmsg) --

  /**
   * @brief Sends multiple datagrams in a single blocking syscall via
   *        sendmmsg(2).
   *
   * Each mmsghdr element must have msg_hdr populated with destination
   * address (msg_name/msg_namelen) and iov buffers. After the call,
   * msgvec[i].msg_len contains the bytes sent for datagram i (i < count).
   *
   * Blocks until at least one datagram is sent. Returns the number of
   * datagrams successfully sent (may be less than vlen if an error occurs
   * mid-batch — partial success is possible).
   *
   * @param msgvec Pre-initialized array of mmsghdr structures.
   * @param vlen   Number of datagrams to send (array length).
   * @pre msgvec != nullptr && vlen > 0
   * @return MmsgResult — count of datagrams sent, 0 on error.
   */
  [[nodiscard]] MmsgResult
  sendmmsg_blocking(struct mmsghdr *msgvec, unsigned int vlen) noexcept {
    assert(msgvec && vlen > 0 &&
           "sendmmsg_blocking: msgvec must not be null and vlen must be > 0");
    int n;
    do {
      n = ::sendmmsg(fd_.get(), msgvec, vlen, MSG_NOSIGNAL);
    } while (n == -1 && errno == EINTR);

    if (n == -1) {
      return {.count = 0, .err_no = errno};
    }
    return {.count = n, .err_no = 0};
  }

  /**
   * @brief Sends multiple datagrams non-blocking via sendmmsg(2).
   *
   * Each mmsghdr element must have msg_hdr populated with destination
   * address (msg_name/msg_namelen) and iov buffers. After the call,
   * msgvec[i].msg_len contains the bytes sent for datagram i (i < count).
   *
   * Returns kWouldBlock (count=0, err_no=0) if the send buffer is full.
   * Partial success is possible: count may be less than vlen if the buffer
   * fills mid-batch.
   *
   * @param msgvec Pre-initialized array of mmsghdr structures.
   * @param vlen   Number of datagrams to send (array length).
   * @pre msgvec != nullptr && vlen > 0
   * @return MmsgResult — count of datagrams sent, 0 on EAGAIN or error.
   */
  [[nodiscard]] MmsgResult
  sendmmsg_nonblocking(struct mmsghdr *msgvec, unsigned int vlen) noexcept {
    assert(msgvec && vlen > 0 &&
           "sendmmsg_nonblocking: msgvec must not be null and vlen must be > 0");
    int n;
    do {
      n = ::sendmmsg(fd_.get(), msgvec, vlen, MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (n == -1 && errno == EINTR);

    if (n == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return {.count = 0, .err_no = 0};
      }
      return {.count = 0, .err_no = errno};
    }
    return {.count = n, .err_no = 0};
  }
};

} // namespace mk::net
