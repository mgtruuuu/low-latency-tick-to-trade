/**
 * @file exchange_md_main.cpp
 * @brief Market data publisher process — reads shared memory, publishes UDP.
 *
 * This is Process 2 of the multi-process simulated exchange. It handles:
 *   - Synthetic market data generation (random walk quotes at fixed intervals)
 *   - Reading fill and BBO events from the shared memory SPSC queue
 *   - Publishing trade and BBO updates via UDP multicast (md_msg_type field)
 *
 * Process 1 (exchange_engine) creates the shared memory region and writes
 * events into it. This process opens the existing region (kOpenExisting)
 * and reads from it.
 *
 * Communication:
 *   exchange_engine ──try_push()──► SharedEventQueue ──try_pop()──►
 *   exchange_md_publisher (this process)
 *
 * Startup protocol:
 *   1. Retry open_shared(kOpenExisting) until the engine creates the region
 *   2. Spin-wait on engine_ready flag (acquire synchronizes with engine's
 *      release, ensuring all initialization is visible)
 *   3. Enter main loop: drain events + publish synthetic ticks
 *
 * Usage:
 *   ./exchange_md_publisher --shm_name=/mk_exchange_events \
 *       --mcast_group=239.255.0.1 --mcast_port=9000
 */

#include "market_data_publisher.hpp"
#include "shared_exchange_queue.hpp"

#include "net/udp_socket.hpp"
#include "sys/log/signal_logger.hpp"
#include "sys/memory/mmap_region.hpp"
#include "sys/nano_clock.hpp"
#include "sys/thread/affinity.hpp"

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

// -- Command-line flags --

ABSL_FLAG(std::string, shm_name, std::string(mk::app::kDefaultShmName),
          "POSIX shared memory name (must match exchange_engine)");
ABSL_FLAG(std::string, mcast_group, "239.255.0.1",
          "Multicast group for market data (Feed A)");
ABSL_FLAG(uint16_t, mcast_port, 9000,
          "Multicast port for market data (Feed A)");
ABSL_FLAG(std::string, mcast_group_b, "",
          "Feed B multicast group (empty = no A/B redundancy)");
ABSL_FLAG(uint16_t, mcast_port_b, 9001, "Feed B multicast port");
ABSL_FLAG(int64_t, tick_interval_us, 100,
          "Synthetic tick interval in microseconds (must be > 0)");
// symbol_count is read from shared memory (set by Engine) — no local flag.
ABSL_FLAG(int32_t, pin_core, -1,
          "CPU core to pin the main loop (-1 = no pinning)");

// -- Global stop flag (signal-safe) --

namespace {

std::atomic_flag g_stop = ATOMIC_FLAG_INIT;

void signal_handler(int /*signum*/) {
  g_stop.test_and_set(std::memory_order_relaxed);
}

} // namespace

// ======================================================================
// main
// ======================================================================

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  const auto shm_name = absl::GetFlag(FLAGS_shm_name);
  const auto mcast_group = absl::GetFlag(FLAGS_mcast_group);
  const auto mcast_port = absl::GetFlag(FLAGS_mcast_port);
  const auto mcast_group_b = absl::GetFlag(FLAGS_mcast_group_b);
  const auto mcast_port_b = absl::GetFlag(FLAGS_mcast_port_b);
  const auto tick_interval_us = absl::GetFlag(FLAGS_tick_interval_us);

  if (tick_interval_us <= 0) {
    mk::sys::log::signal_log("[MD_PUB] Invalid --tick_interval_us=",
                             tick_interval_us, " (must be > 0)\n");
    return 1;
  }
  const auto tick_interval_ns = tick_interval_us * 1000;

  // -- Signal handling --
  struct sigaction sa{};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  // -- CPU core pinning --
  const auto pin_core = absl::GetFlag(FLAGS_pin_core);
  if (pin_core >= 0) {
    auto err = mk::sys::thread::pin_current_thread(
        static_cast<std::uint32_t>(pin_core));
    if (err == 0) {
      mk::sys::log::signal_log("[MD_PUB] Pinned to core ", pin_core, '\n');
    } else {
      mk::sys::log::signal_log("[MD_PUB] Failed to pin to core ", pin_core,
                               " (errno=", err, ")\n");
    }
  }

  // -- Open shared memory (retry until engine creates it) --
  //
  // The engine process creates the shared memory object. This process
  // opens the EXISTING object. If the engine hasn't started yet, the
  // open will fail — we retry with a brief sleep (cold path, 100ms).
  mk::sys::log::signal_log("[MD_PUB] Waiting for exchange_engine to create "
                           "shared memory: ",
                           shm_name.c_str(), '\n');

  std::optional<mk::sys::memory::MmapRegion> shm_region;
  while (!g_stop.test(std::memory_order_relaxed)) {
    shm_region = mk::sys::memory::MmapRegion::open_shared(
        shm_name, sizeof(mk::app::SharedExchangeRegion),
        mk::sys::memory::ShmMode::kOpenExisting,
        mk::sys::memory::PrefaultPolicy::kPopulateRead);
    if (shm_region) {
      break;
    }
    // Engine not ready yet — brief cold-path sleep.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!shm_region) {
    mk::sys::log::signal_log("[MD_PUB] Interrupted before shared memory "
                             "became available\n");
    return 1;
  }

  // Cast (not placement new!) — the engine already constructed the region.
  // This process sees the same physical pages through its own virtual mapping.
  auto *shared = reinterpret_cast<
      mk::app::SharedExchangeRegion *>( // NOLINT(*-pro-type-reinterpret-cast)
      shm_region->data());

  // -- Wait for engine readiness --
  //
  // The engine stores engine_ready = 1 with memory_order_release AFTER
  // initializing the queue and registering symbols. Our acquire load
  // guarantees we see all those preceding writes — this is the
  // acquire/release synchronization pattern.
  mk::sys::log::signal_log("[MD_PUB] Shared memory mapped. Waiting for "
                           "engine_ready signal...\n");
  while (!g_stop.test(std::memory_order_relaxed) &&
         shared->engine_ready.load(std::memory_order_acquire) == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (g_stop.test(std::memory_order_relaxed)) {
    return 1;
  }

  // Read symbol_count from shared memory (set by Engine before engine_ready).
  // This ensures MD publisher always matches the engine's configuration,
  // preventing silent event drops from --symbol_count mismatch.
  const auto symbol_count =
      shared->symbol_count.load(std::memory_order_relaxed);
  if (symbol_count == 0 || symbol_count > 2) {
    mk::sys::log::signal_log("[MD_PUB] Invalid symbol_count=", symbol_count,
                             " from engine\n");
    return 1;
  }

  mk::sys::log::signal_log("[MD_PUB] Engine ready. Starting market data "
                           "publisher.\n");

  // -- Create UDP multicast socket --
  const int udp_fd =
      ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (udp_fd < 0) {
    mk::sys::log::signal_log("[MD_PUB] UDP socket() failed\n");
    return 1;
  }
  mk::net::UdpSocket udp_sock(udp_fd);
  (void)udp_sock.set_multicast_ttl(1);
  (void)udp_sock.set_multicast_loop(true);

  // Feed A destination.
  sockaddr_in feed_a_dest{};
  feed_a_dest.sin_family = AF_INET;
  feed_a_dest.sin_port = htons(mcast_port);
  if (inet_pton(AF_INET, mcast_group.c_str(), &feed_a_dest.sin_addr) != 1) {
    mk::sys::log::signal_log(
        "[MD_PUB] Invalid --mcast_group: ", mcast_group.c_str(), '\n');
    return 1;
  }

  // Feed B destination (optional).
  sockaddr_in feed_b_storage{};
  const sockaddr_in *feed_b_dest = nullptr;
  if (!mcast_group_b.empty()) {
    feed_b_storage.sin_family = AF_INET;
    feed_b_storage.sin_port = htons(mcast_port_b);
    if (inet_pton(AF_INET, mcast_group_b.c_str(), &feed_b_storage.sin_addr) !=
        1) {
      mk::sys::log::signal_log(
          "[MD_PUB] Invalid --mcast_group_b: ", mcast_group_b.c_str(), '\n');
      return 1;
    }
    feed_b_dest = &feed_b_storage;
  }

  // -- Create MarketDataPublishers --
  std::uint64_t shared_seq = 1;
  mk::app::MarketDataPublisher pub1(udp_sock, /*symbol_id=*/1, shared_seq,
                                    feed_a_dest, feed_b_dest);
  mk::app::MarketDataPublisher pub2(udp_sock, /*symbol_id=*/2, shared_seq,
                                    feed_a_dest, feed_b_dest);
  std::array<mk::app::MarketDataPublisher<> *, 2> publishers = {&pub1, &pub2};

  mk::sys::log::signal_log("[MD_PUB] Publishing: mcast_a=", mcast_group.c_str(),
                           ":", mcast_port, " symbols=", symbol_count,
                           " tick_interval=", tick_interval_us, "us\n");

  // Status log interval.
  const auto log_every_ticks = std::max<std::uint64_t>(
      1, static_cast<std::uint64_t>(5'000'000 / tick_interval_us));

  // -- Main event loop --
  auto last_tick = mk::sys::monotonic_nanos();
  std::uint64_t tick_count = 0;
  std::uint64_t events_consumed = 0;

  while (!g_stop.test(std::memory_order_relaxed) &&
         shared->shutdown.load(std::memory_order_relaxed) == 0) {

    // 1. Drain events from shared memory queue.
    //    The engine pushes kFill and kBBOUpdate events. The wire format
    //    has md_msg_type (kTrade vs kBBOUpdate) so downstream can
    //    distinguish them and only update BBO state from BBO packets.
    mk::app::ExchangeEvent event{};
    while (shared->md_event_queue.try_pop(event)) {
      if (event.symbol_id < 1 || event.symbol_id > symbol_count) {
        mk::sys::log::signal_log(
            "[MD_PUB] Unexpected symbol_id=", event.symbol_id, ", dropping\n");
        continue;
      }
      auto *pub = publishers[event.symbol_id - 1];
      if (event.type == mk::app::EventType::kFill) {
        pub->publish_trade(event.side, event.price, event.qty);
      } else if (event.type == mk::app::EventType::kBBOUpdate) {
        pub->publish_update(event.side, event.price, event.qty);
      }
      ++events_consumed;
    }

    // 2. Synthetic tick generation (independent of engine events).
    //    Same anchor-based timing as single-process mode.
    auto now = mk::sys::monotonic_nanos();
    if (now - last_tick >= tick_interval_ns) {
      pub1.publish_tick();
      if (symbol_count >= 2) {
        pub2.publish_tick();
      }
      last_tick += tick_interval_ns;
      ++tick_count;

      if (tick_count % log_every_ticks == 0) {
        mk::sys::log::signal_log("[MD_PUB] ticks=", tick_count,
                                 " seq=", shared_seq,
                                 " events=", events_consumed, '\n');
      }
    }
  }

  // Signal shutdown to other processes.
  shared->shutdown.store(1U, std::memory_order_relaxed);
  mk::sys::log::signal_log("[MD_PUB] Shutdown. ticks=", tick_count,
                           " events_consumed=", events_consumed, '\n');
  return 0;
}
