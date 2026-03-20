/**
 * @file main.cpp
 * @brief Simulated exchange — two operating modes.
 *
 * Mode 1: Live (default)
 *   Generates synthetic market data via random walk and publishes via UDP
 *   multicast. Orders processed through MatchingEngine.
 *
 * Mode 2: Replay (--replay_file=path)
 *   Reads pre-generated binary tick data (from generate_ticks) and replays
 *   at original timing (or accelerated with --speed). Orders get immediate
 *   ack + fill (no matching engine — data is read-only).
 *
 * Both modes:
 *   - TCP server for order entry (TLV protocol via message_codec.hpp)
 *   - UDP multicast for market data
 *   - Single-threaded event loop
 *
 * Usage:
 *   # Live mode
 *   ./simulated_exchange --tcp_port=8888 \
 *       --mcast_group=239.255.0.1 --mcast_port=9000
 *
 *   # Replay mode
 *   ./simulated_exchange --tcp_port=8888 \
 *       --mcast_group=239.255.0.1 --mcast_port=9000 \
 *       --replay_file=market_data.bin --speed=1.0
 */

#include "market_data_publisher.hpp"
#include "message_codec_framer.hpp"
#include "order_gateway.hpp"
#include "replay_order_gateway.hpp"
#include "tick_file_reader.hpp"

#include "shared/protocol.hpp"
#include "shared/protocol_codec.hpp"
#include "shared/tick_data.hpp"

#include "net/tcp_server.hpp"
#include "net/udp_socket.hpp"
#include "sys/hardware_constants.hpp"
#include "sys/log/signal_logger.hpp"
#include "sys/nano_clock.hpp"
#include "sys/thread/affinity.hpp"

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"

#include <atomic>
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>

// -- Command-line flags --

ABSL_FLAG(uint16_t, tcp_port, 8888, "TCP port for order gateway");
ABSL_FLAG(std::string, mcast_group, "239.255.0.1",
          "Multicast group for market data (Feed A)");
ABSL_FLAG(uint16_t, mcast_port, 9000, "Multicast port for market data (Feed A)");
ABSL_FLAG(std::string, mcast_group_b, "",
          "Feed B multicast group (empty = no A/B redundancy)");
ABSL_FLAG(uint16_t, mcast_port_b, 9001, "Feed B multicast port");

// Live mode flags.
ABSL_FLAG(int64_t, tick_interval_us, 1000,
          "Market data publish interval in microseconds (live mode, must be > 0)");
ABSL_FLAG(uint32_t, symbol_count, 2,
          "Number of symbols to publish (live mode, max 2)");

// Replay mode flags.
ABSL_FLAG(std::string, replay_file, "",
          "Path to binary tick file (enables replay mode)");
ABSL_FLAG(double, speed, 1.0,
          "Replay speed multiplier (1.0 = real-time, 0 = max speed)");
ABSL_FLAG(int32_t, pin_core, -1,
          "CPU core to pin the main event loop (-1 = no pinning)");

// -- Global stop flag (signal-safe) --

namespace {
std::atomic_flag g_stop = ATOMIC_FLAG_INIT;

void signal_handler(int /*signum*/) {
  g_stop.test_and_set(std::memory_order_relaxed);
}

// -- TcpServer configuration (shared between modes) --

constexpr int kMaxConns = 64;
constexpr std::size_t kRxBufSize = 8192;
constexpr std::size_t kTxBufSize = 16384;
static_assert(kTxBufSize >= mk::app::OrderGateway<>::kMaxResponseBytes,
              "kTxBufSize too small for OrderGateway worst-case response");
static_assert(kTxBufSize >= mk::app::ReplayOrderGateway::kMaxResponseBytes,
              "kTxBufSize too small for ReplayOrderGateway response");

} // namespace

// ======================================================================
// Live mode handler — uses OrderGateway with MatchingEngine
// ======================================================================

struct LiveHandler {
  mk::app::OrderGateway<> &gateway;
  std::array<std::uint16_t, kMaxConns> sessions_by_conn{};

  bool on_connect(mk::net::ConnId id) noexcept {
    if (id < 0 || id >= kMaxConns) [[unlikely]] {
      mk::sys::log::signal_log("[EXCHANGE] Client connect out of range: fd=",
                               id, '\n');
      return false;
    }
    auto session = gateway.on_client_connect();
    sessions_by_conn[static_cast<std::size_t>(id)] = session;
    mk::sys::log::signal_log("[EXCHANGE] Client connected: fd=", id,
                              " session=", session, '\n');
    return true;
  }

  std::size_t on_data(mk::net::ConnId id,
                      std::span<const std::byte> payload,
                      std::span<std::byte> tx_space) noexcept {
    if (id < 0 || id >= kMaxConns) [[unlikely]] {
      return 0;
    }
    const auto session = sessions_by_conn[static_cast<std::size_t>(id)];
    return gateway.on_message(session, payload, tx_space);
  }

  void on_disconnect(mk::net::ConnId id) noexcept {
    if (id < 0 || id >= kMaxConns) [[unlikely]] {
      mk::sys::log::signal_log(
          "[EXCHANGE] Client disconnect out of range: fd=", id, '\n');
      return;
    }
    auto &session = sessions_by_conn[static_cast<std::size_t>(id)];
    if (session != 0) {
      gateway.on_client_disconnect(session);
      session = 0;
    }
    mk::sys::log::signal_log("[EXCHANGE] Client disconnected: fd=", id, '\n');
  }
};

// ======================================================================
// Replay mode handler — uses ReplayOrderGateway (immediate fill)
// ======================================================================

struct ReplayHandler {
  mk::app::ReplayOrderGateway &gateway;

  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  bool on_connect(mk::net::ConnId id) noexcept {
    mk::sys::log::signal_log("[EXCHANGE] Client connected: fd=", id, '\n');
    return true;
  }

  std::size_t on_data(mk::net::ConnId /*id*/,
                      std::span<const std::byte> payload,
                      std::span<std::byte> tx_space) noexcept {
    return gateway.on_message(payload, tx_space);
  }

  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  void on_disconnect(mk::net::ConnId id) noexcept {
    mk::sys::log::signal_log("[EXCHANGE] Client disconnected: fd=", id, '\n');
  }
};

using LiveServer =
    mk::net::TcpServer<LiveHandler, kMaxConns, kRxBufSize, kTxBufSize,
                        mk::app::MessageCodecFramer>;
using ReplayServer =
    mk::net::TcpServer<ReplayHandler, kMaxConns, kRxBufSize, kTxBufSize,
                        mk::app::MessageCodecFramer>;

// ======================================================================
// Internal helpers (anonymous namespace for internal linkage)
// ======================================================================

namespace {

void *allocate_server_buffer(std::size_t required) noexcept {
  constexpr std::size_t kAlign = mk::sys::kCacheLineSize;
  auto alloc_size = (required + kAlign - 1) / kAlign * kAlign;
  return std::aligned_alloc(kAlign, alloc_size);
}

// Publishes a single Tick as 2 UDP datagrams (bid + ask).
// Converts from host byte order (mmap'd file) to big-endian wire format.
void publish_replay_tick(mk::net::UdpSocket &sock,
                         const sockaddr_in &dest,
                         const mk::app::Tick &tick,
                         std::uint64_t &seq_num,
                         std::array<std::byte, 128> &buf) noexcept {
  // Tick file stores symbol_id as 0-based index for compact replay data.
  // Wire protocol uses 1-based symbol_id (0 is reserved as invalid).
  const std::uint32_t wire_symbol_id = tick.symbol_id + 1;

  // Bid update.
  mk::app::MarketDataUpdate bid;
  bid.seq_num = seq_num++;
  bid.symbol_id = wire_symbol_id;
  bid.side = mk::algo::Side::kBid;
  bid.price = tick.bid_price;
  bid.qty = tick.bid_qty;
  bid.exchange_ts = mk::sys::monotonic_nanos();

  auto bytes = mk::app::serialize_market_data(buf, bid);
  if (bytes > 0) {
    (void)sock.sendto_nonblocking(
        reinterpret_cast<const char *>(buf.data()), bytes, dest);
  }

  // Ask update.
  mk::app::MarketDataUpdate ask;
  ask.seq_num = seq_num++;
  ask.symbol_id = wire_symbol_id;
  ask.side = mk::algo::Side::kAsk;
  ask.price = tick.ask_price;
  ask.qty = tick.ask_qty;
  ask.exchange_ts = mk::sys::monotonic_nanos();

  bytes = mk::app::serialize_market_data(buf, ask);
  if (bytes > 0) {
    (void)sock.sendto_nonblocking(
        reinterpret_cast<const char *>(buf.data()), bytes, dest);
  }
}

// ======================================================================
// Live mode main loop
// ======================================================================

int run_live(mk::net::UdpSocket &udp_sock, const sockaddr_in &feed_a_dest,
             const sockaddr_in *feed_b_dest, void *server_buf) {
  const auto tick_interval_us = absl::GetFlag(FLAGS_tick_interval_us);
  auto symbol_count = absl::GetFlag(FLAGS_symbol_count); // mutable: clamped below
  const auto tcp_port = absl::GetFlag(FLAGS_tcp_port);
  const auto mcast_group = absl::GetFlag(FLAGS_mcast_group);
  const auto mcast_port = absl::GetFlag(FLAGS_mcast_port);

  if (tick_interval_us <= 0) {
    mk::sys::log::signal_log("[EXCHANGE] Invalid --tick_interval_us=",
                             tick_interval_us, " (must be > 0)\n");
    return 1;
  }
  const auto tick_interval_ns = tick_interval_us * 1000;
  // Status log every ~5s worth of ticks (at least once per tick for very slow rates).
  const auto log_every_ticks = std::max<std::uint64_t>(
      1, static_cast<std::uint64_t>(5'000'000 / tick_interval_us));

  // Clamp symbol count to [1, 2].
  symbol_count = std::clamp(symbol_count, 1U, 2U);

  // Shared sequence number counter across all publishers (ITCH model).
  std::uint64_t shared_seq = 1;

  // Create publishers — one per symbol, sharing seq_num and socket.
  mk::app::MarketDataPublisher pub1(udp_sock, /*symbol_id=*/1, shared_seq,
                                    feed_a_dest, feed_b_dest);
  mk::app::MarketDataPublisher pub2(udp_sock, /*symbol_id=*/2, shared_seq,
                                    feed_a_dest, feed_b_dest);

  // Create gateway and register symbols.
  mk::app::OrderGateway<> gateway;
  gateway.register_symbol(1, pub1);
  if (symbol_count >= 2) {
    gateway.register_symbol(2, pub2);
  }

  LiveHandler handler{.gateway = gateway};
  const mk::net::TcpServerConfig config{
      .port = tcp_port,
      .backlog = 16,
      .nodelay = true,
      .busy_spin = false,
      .pin_core = -1,
      .alloc_guard = false,
  };
  auto server = std::make_unique<LiveServer>(
      handler, config, server_buf, LiveServer::required_buffer_size(),
      mk::app::MessageCodecFramer{});

  if (!server->listen() || !server->start()) {
    mk::sys::log::signal_log("[EXCHANGE] Failed to start server\n");
    return 1;
  }

  mk::sys::log::signal_log("[EXCHANGE] Live mode: TCP port=", server->port(),
                           " mcast_a=", mcast_group.c_str(), ":", mcast_port,
                           " symbols=", symbol_count,
                           " feed_b=", (feed_b_dest != nullptr ? "yes" : "no"),
                           " tick_interval=", tick_interval_us, "us\n");

  auto last_tick = mk::sys::monotonic_nanos();
  std::uint64_t tick_count = 0;

  while (!g_stop.test(std::memory_order_relaxed)) {
    server->poll_once(1);

    auto now = mk::sys::monotonic_nanos();
    if (now - last_tick >= tick_interval_ns) {
      pub1.publish_tick();
      if (symbol_count >= 2) {
        pub2.publish_tick();
      }
      // Anchor-based advancement: prevents cumulative jitter drift.
      // Using `last_tick = now` would cause each tick's detection delay
      // to shift the baseline, accumulating over thousands of ticks.
      last_tick += tick_interval_ns;
      ++tick_count;

      if (tick_count % log_every_ticks == 0) {
        mk::sys::log::signal_log(
            "[EXCHANGE] ticks=", tick_count, " seq=", shared_seq,
            " mid1=", pub1.mid_price(),
            " book1=", gateway.engine(1).book().total_orders(), '\n');
      }
    }
  }

  mk::sys::log::signal_log("[EXCHANGE] Shutdown. Total ticks=", tick_count,
                           '\n');
  return 0;
}

// ======================================================================
// Replay mode main loop
// ======================================================================

int run_replay(mk::net::UdpSocket &udp_sock, const sockaddr_in &mcast_dest,
               void *server_buf, mk::app::TickFileReader &reader) {
  auto tcp_port = absl::GetFlag(FLAGS_tcp_port);
  auto speed = absl::GetFlag(FLAGS_speed);
  auto mcast_group = absl::GetFlag(FLAGS_mcast_group);
  auto mcast_port = absl::GetFlag(FLAGS_mcast_port);

  mk::app::ReplayOrderGateway gateway;

  ReplayHandler handler{gateway};
  const mk::net::TcpServerConfig config{
      .port = tcp_port,
      .backlog = 16,
      .nodelay = true,
      .busy_spin = false,
      .pin_core = -1,
      .alloc_guard = false,
  };
  auto server = std::make_unique<ReplayServer>(
      handler, config, server_buf, ReplayServer::required_buffer_size(),
      mk::app::MessageCodecFramer{});

  if (!server->listen() || !server->start()) {
    mk::sys::log::signal_log("[EXCHANGE] Failed to start server\n");
    return 1;
  }

  mk::sys::log::signal_log("[EXCHANGE] Replay mode: TCP port=", server->port(),
                           " mcast=", mcast_group.c_str(), ":", mcast_port,
                           " ticks=", reader.tick_count(),
                           " symbols=", reader.symbol_count(), '\n');
  if (speed > 0.0) {
    // Log speed as integer percentage to avoid floating-point in signal_log.
    auto speed_pct = static_cast<std::uint64_t>(speed * 100.0);
    mk::sys::log::signal_log("[EXCHANGE] Replay speed: ", speed_pct, "%\n");
  } else {
    mk::sys::log::signal_log("[EXCHANGE] Replay speed: max (no delay)\n");
  }

  auto ticks = reader.ticks();
  std::uint64_t seq_num = 1;
  std::array<std::byte, 128> send_buf{};
  std::size_t tick_idx = 0;
  std::uint64_t published = 0;

  // Wall-clock anchor for timing: map tick[0].timestamp_ns to "now".
  auto wall_start = mk::sys::monotonic_nanos();
  auto data_start = ticks.empty() ? 0ULL : ticks[0].timestamp_ns;

  while (!g_stop.test(std::memory_order_relaxed) && tick_idx < ticks.size()) {
    server->poll_once(0); // Non-blocking — check TCP events.

    // -- Timing: wait until it's time to publish the next tick --
    const auto &tick = ticks[tick_idx];
    auto data_offset = tick.timestamp_ns - data_start;

    if (speed > 0.0) {
      // Scale the data timestamp offset by speed.
      auto wall_target =
          wall_start +
          static_cast<std::uint64_t>(static_cast<double>(data_offset) / speed);
      auto now = mk::sys::monotonic_nanos();

      if (std::cmp_less(now, wall_target)) {
        // Not time yet. Keep polling TCP while waiting.
        // For short waits (< 1ms), just spin. For longer, use poll timeout.
        auto remaining_ns = wall_target - now;
        if (remaining_ns > 1'000'000) {
          // Use epoll timeout to avoid burning CPU during long gaps.
          auto timeout_ms =
              std::min(static_cast<int>(remaining_ns / 1'000'000), 10);
          server->poll_once(timeout_ms);
        }
        continue;
      }
    }

    // -- Publish this tick --
    publish_replay_tick(udp_sock, mcast_dest, tick, seq_num, send_buf);
    ++tick_idx;
    ++published;

    // Periodic status log.
    if (published % 100'000 == 0) {
      auto pct = tick_idx * 100 / ticks.size();
      mk::sys::log::signal_log("[EXCHANGE] Replay: ", published, "/",
                                ticks.size(), " (", pct, "%) seq=", seq_num,
                                '\n');
    }
  }

  if (tick_idx >= ticks.size()) {
    mk::sys::log::signal_log("[EXCHANGE] Replay complete: ", published,
                             " ticks published\n");
    // Keep server alive briefly so pipeline can finish processing.
    auto shutdown_start = mk::sys::monotonic_nanos();
    while (!g_stop.test(std::memory_order_relaxed) &&
           mk::sys::monotonic_nanos() - shutdown_start < 2'000'000'000ULL) {
      server->poll_once(100);
    }
  }

  mk::sys::log::signal_log("[EXCHANGE] Shutdown. Published ", published,
                           " ticks, seq=", seq_num, '\n');
  return 0;
}

} // namespace

// ======================================================================
// main
// ======================================================================

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  auto mcast_group = absl::GetFlag(FLAGS_mcast_group);
  auto mcast_port = absl::GetFlag(FLAGS_mcast_port);
  auto mcast_group_b = absl::GetFlag(FLAGS_mcast_group_b);
  auto mcast_port_b = absl::GetFlag(FLAGS_mcast_port_b);
  auto replay_file = absl::GetFlag(FLAGS_replay_file);

  // -- Signal handling --
  struct sigaction sa {};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  // -- CPU core pinning (single-threaded event loop) --
  const auto pin_core = absl::GetFlag(FLAGS_pin_core);
  if (pin_core >= 0) {
    auto err = mk::sys::thread::pin_current_thread(
        static_cast<std::uint32_t>(pin_core));
    if (err == 0) {
      mk::sys::log::signal_log("[EXCHANGE] Pinned to core ", pin_core, '\n');
    } else {
      mk::sys::log::signal_log("[EXCHANGE] Failed to pin to core ", pin_core,
                               " (errno=", err, ")\n");
    }
  }

  // -- Create UDP multicast socket --
  const int udp_fd =
      ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (udp_fd < 0) {
    mk::sys::log::signal_log("[EXCHANGE] UDP socket() failed\n");
    return 1;
  }
  mk::net::UdpSocket udp_sock(udp_fd);
  (void)udp_sock.set_multicast_ttl(1);
  (void)udp_sock.set_multicast_loop(true);

  // Feed A destination.
  sockaddr_in feed_a_dest{};
  feed_a_dest.sin_family = AF_INET;
  feed_a_dest.sin_port = htons(mcast_port);
  inet_pton(AF_INET, mcast_group.c_str(), &feed_a_dest.sin_addr);

  // Feed B destination (optional — for A/B redundancy).
  sockaddr_in feed_b_storage{};
  const sockaddr_in *feed_b_dest = nullptr;
  if (!mcast_group_b.empty()) {
    feed_b_storage.sin_family = AF_INET;
    feed_b_storage.sin_port = htons(mcast_port_b);
    inet_pton(AF_INET, mcast_group_b.c_str(), &feed_b_storage.sin_addr);
    feed_b_dest = &feed_b_storage;
  }

  // -- Allocate TcpServer buffer --
  // Both server types have the same template parameters, so same buffer size.
  static_assert(LiveServer::required_buffer_size() ==
                ReplayServer::required_buffer_size());
  void *server_buf =
      allocate_server_buffer(LiveServer::required_buffer_size());
  if (!server_buf) {
    mk::sys::log::signal_log("[EXCHANGE] Failed to allocate server buffer\n");
    return 1;
  }

  // -- Dispatch to mode --
  int result = 0;
  if (replay_file.empty()) {
    result = run_live(udp_sock, feed_a_dest, feed_b_dest, server_buf);
  } else {
    auto reader = mk::app::TickFileReader::open(replay_file.c_str());
    if (!reader) {
      std::free(server_buf); // NOLINT(cppcoreguidelines-no-malloc)
      return 1;
    }
    result = run_replay(udp_sock, feed_a_dest, server_buf, *reader);
  }

  std::free(server_buf); // NOLINT(cppcoreguidelines-no-malloc)
  return result;
}
