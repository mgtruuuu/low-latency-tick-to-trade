/**
 * @file mock_udp_socket.hpp
 * @brief Mock UDP socket for simulated exchange tests.
 *
 * Satisfies net::UdpSendable concept. Records all sent datagrams for
 * inspection instead of performing real I/O. Shared across
 * order_gateway_test, exchange_core_test, and market_data_publisher_test.
 */

#pragma once

#include "net/udp_socket_concept.hpp"

#include <arpa/inet.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mk::app::test {

struct MockUdpSocket {
  enum class SendtoStatus { kOk, kWouldBlock, kError };

  struct SendtoResult {
    SendtoStatus status = SendtoStatus::kError;
    int err_no = 0;
  };

  struct Packet {
    std::vector<std::byte> data;
    sockaddr_in dest;
  };

  // All sent packets, available for test assertions.
  std::vector<Packet> sent_packets;

  // If true, sendto_nonblocking returns kError to simulate failure.
  bool fail_sends = false;

  SendtoResult sendto_nonblocking(const char *buf, std::size_t len,
                                  const sockaddr_in &dest) noexcept {
    if (fail_sends) {
      return {.status = SendtoStatus::kError, .err_no = 11 /* EAGAIN */};
    }

    sent_packets.push_back(
        {.data = {reinterpret_cast<const std::byte *>(buf),
                  reinterpret_cast<const std::byte *>(buf) + len},
         .dest = dest});
    return {.status = SendtoStatus::kOk, .err_no = 0};
  }
};

// Verify MockUdpSocket satisfies the concept at compile time.
static_assert(net::UdpSendable<MockUdpSocket>,
              "MockUdpSocket must satisfy UdpSendable");

inline sockaddr_in make_test_addr(const char *ip, std::uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, ip, &addr.sin_addr);
  return addr;
}

} // namespace mk::app::test
