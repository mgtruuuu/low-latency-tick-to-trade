/**
 * @file udp_socket_concept.hpp
 * @brief UdpSendable concept — contract for UDP socket wrappers.
 *
 * Constrains the UdpSock template parameter used by MarketDataPublisher
 * and OrderGateway. Requires only the "send" side of UDP — the minimum
 * needed for market data publishing.
 *
 * mk::net::UdpSocket satisfies this concept. Test mocks must expose
 * the same interface.
 *
 * Design rationale (concept vs #ifdef vs CRTP vs virtual):
 * See studies/system_design/docs/compile_time_polymorphism.md §7.
 */

#pragma once

#include <concepts>
#include <cstddef>

struct sockaddr_in;

namespace mk::net {

// =============================================================================
// UdpSendable concept — contract for UDP socket wrappers used by
// MarketDataPublisher and OrderGateway.
// =============================================================================
//
// A UDP socket wrapper must expose:
//   - SendtoStatus    — nested enum with at least kOk member.
//   - SendtoResult    — nested struct with { SendtoStatus status; int err_no; }.
//   - sendto_nonblocking(buf, len, dest) noexcept — send a datagram.
//
// [Educational note]
// This concept constrains only the "send" side of UDP — the minimum
// required by MarketDataPublisher. A broader UdpSocket concept
// (including recvfrom, multicast join, etc.) could be defined if
// needed by other templates.
template <typename US>
concept UdpSendable = requires(US sock, const char *buf, std::size_t len,
                               const struct sockaddr_in &dest) {
  typename US::SendtoStatus;
  typename US::SendtoResult;
  { US::SendtoStatus::kOk } -> std::same_as<typename US::SendtoStatus>;
  { sock.sendto_nonblocking(buf, len, dest) } noexcept
      -> std::same_as<typename US::SendtoResult>;
};

} // namespace mk::net
