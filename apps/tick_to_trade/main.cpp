/**
 * @file main.cpp
 * @brief Tick-to-trade trading pipeline — cold-path launcher.
 *
 * End-to-end trading pipeline demonstrating wire-to-wire latency
 * measurement. Receives market data via UDP multicast, evaluates a
 * trading strategy, and sends orders via TCP to a simulated exchange.
 *
 * Architecture:
 *   Launcher (main thread) + two dedicated worker threads:
 *     - main():         Cold-path launcher — config, memory allocation,
 *                       socket setup, thread lifecycle, shutdown stats.
 *     - MdFeedThread:   Core-pinned, epoll {UDP-A, UDP-B} → recvmmsg →
 *                       FeedHandler → SPSC push.
 *     - StrategyThread: Core-pinned, SPSC drain → Strategy → risk check →
 *                       TCP orders. Handles heartbeat, reconnection,
 *                       order timeouts, kill switch.
 *
 *   Pipeline stages per market data tick:
 *     [UDP recv] → [FeedHandler] → [SPSC] → [Strategy] → [OrderManager] → [TCP
 * send] t0           t1                       t2             t3              t4
 *
 *   Each stage boundary is instrumented with rdtsc() for sub-microsecond
 *   latency measurement. Queue hop latency between threads is measured.
 *
 * Usage:
 *   ./tick_to_trade --exchange_host=127.0.0.1 --exchange_port=8888 \
 *       --mcast_group=239.255.0.1 --mcast_port=9000 \
 *       --pin_core_strategy=2 --pin_core_md=1 --pin_core_logger=3
 *
 *   With A/B feed redundancy (optional):
 *   ./tick_to_trade --exchange_host=127.0.0.1 --exchange_port=8888 \
 *       --mcast_group=239.255.0.1 --mcast_port=9000 \
 *       --mcast_group_b=239.255.0.2 --mcast_port_b=9001 \
 *       --pin_core_strategy=2 --pin_core_md=1 --pin_core_logger=3
 *
 *   Press Ctrl+C to stop. Latency report prints on shutdown.
 */

#include "feed_handler.hpp"
#include "latency_tracker.hpp"
#include "md_ctx.hpp"
#include "md_feed_thread.hpp"
#include "md_types.hpp"
#include "order_manager.hpp"
#include "spread_strategy.hpp"
#include "strategy_ctx.hpp"
#include "strategy_thread.hpp"
#include "tcp_connection.hpp"

#include "net/epoll_wrapper.hpp"
#include "net/scoped_fd.hpp"
#include "net/tcp_socket.hpp"
#include "net/udp_socket.hpp"
#include "pipeline_log_entry.hpp"
#include "pipeline_log_formatter.hpp"
#include "sys/log/async_drain_loop.hpp"
#include "sys/log/signal_logger.hpp"
#include "sys/memory/global_new_delete.hpp"
#include "sys/memory/mmap_utils.hpp"
#include "sys/nano_clock.hpp"
#include "sys/thread/affinity.hpp"

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"

#include <arpa/inet.h>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

// Active strategy for this binary. Change the template argument to deploy
// a different strategy (e.g. SpreadStrategy<4> for a 4-symbol universe).
using ActiveStrategy = mk::app::SpreadStrategy<2>;

// -- Command-line flags --

ABSL_FLAG(std::string, exchange_host, "127.0.0.1", "Exchange TCP host address");
ABSL_FLAG(uint16_t, exchange_port, 8888, "Exchange TCP port");
ABSL_FLAG(std::string, mcast_group, "239.255.0.1",
          "Market data multicast group (Feed A)");
ABSL_FLAG(uint16_t, mcast_port, 9000, "Market data multicast port (Feed A)");
ABSL_FLAG(std::string, mcast_group_b, "",
          "Feed B multicast group (empty = no A/B redundancy)");
ABSL_FLAG(uint16_t, mcast_port_b, 9001, "Feed B multicast port");
ABSL_FLAG(int32_t, pin_core_strategy, -1,
          "CPU core for the strategy thread (-1 = no pinning)");
ABSL_FLAG(int64_t, spread_threshold, 1000,
          "Spread threshold for strategy signal (in price ticks)");
ABSL_FLAG(uint32_t, order_qty, 10, "Default order quantity");
ABSL_FLAG(uint32_t, max_outstanding, 1024,
          "Maximum outstanding orders (global)");
ABSL_FLAG(uint32_t, max_outstanding_per_symbol, 16,
          "Maximum outstanding orders per symbol");
ABSL_FLAG(int64_t, max_position, 100, "Maximum absolute net position");
ABSL_FLAG(uint32_t, max_order_size, 1000, "Maximum single order quantity");
ABSL_FLAG(
    int64_t, max_notional, 50000000,
    "Maximum notional per order (price * qty). Fat-finger protection: "
    "set to ~5x expected notional to catch absurd orders while allowing "
    "normal price drift. e.g. qty=10 at $100 = 10,000,000; limit=50,000,000.");
ABSL_FLAG(uint32_t, max_orders_per_sec, 100,
          "Maximum orders per second (rate limit)");
ABSL_FLAG(int32_t, order_timeout_ms, 5000,
          "Order ack timeout in milliseconds. Orders without ack/fill within "
          "this window are automatically cancelled.");
ABSL_FLAG(
    int32_t, numa_node, -1,
    "NUMA node for memory binding "
    "(-1 = auto mode: use pin_core_strategy's node when pin_core_strategy>=0, "
    "otherwise no explicit binding)");
ABSL_FLAG(std::string, nic_iface, "",
          "Network interface for NUMA locality check (empty = skip)");
ABSL_FLAG(int64_t, stats_interval, 10,
          "Periodic stats dump interval in seconds (0 = disabled)");
ABSL_FLAG(int32_t, pin_core_md, -1,
          "CPU core for the MD feed thread (-1 = no pinning)");
ABSL_FLAG(int32_t, pin_core_logger, -1,
          "CPU core for the async logger thread (-1 = no pinning)");
ABSL_FLAG(bool, strict_affinity, false,
          "Fail fast unless hot-path threads (strategy/md) use explicit core "
          "pinning and NUMA topology is resolvable");
ABSL_FLAG(uint32_t, tcp_rx_size, 8192,
          "Strategy thread TCP recv buffer size (bytes)");
ABSL_FLAG(uint32_t, tcp_tx_size, 4096,
          "Strategy thread TCP send buffer size (bytes)");
ABSL_FLAG(uint32_t, scratch_size, 128,
          "Strategy thread serialization scratch buffer size (bytes). "
          "Must be >= the largest message wire size in the protocol "
          "(currently kFillReportWireSize = 40 bytes).");

// -- Global stop flag (signal-safe) --

namespace {
std::atomic_flag g_stop = ATOMIC_FLAG_INIT;
std::atomic_flag g_kill_switch = ATOMIC_FLAG_INIT;

// Bring extracted types into the anonymous namespace for unqualified use.
using mk::app::connect_to_exchange;
using mk::app::ConnectionState;
using mk::app::MdEpollSlot;
using mk::app::MdFeedSource;
using mk::app::MdToStrategyQueue;
using mk::app::QueuedUpdate;

void signal_handler(int signum) {
  if (signum == SIGUSR1) {
    // SIGUSR1: explicit kill switch trigger (e.g., from monitoring system).
    g_kill_switch.test_and_set(std::memory_order_relaxed);
  } else {
    // SIGINT/SIGTERM: first press triggers kill switch for graceful cancel-all.
    // Second press forces immediate exit.
    if (g_kill_switch.test(std::memory_order_relaxed)) {
      g_stop.test_and_set(
          std::memory_order_relaxed); // Already killing — force stop.
    } else {
      g_kill_switch.test_and_set(std::memory_order_relaxed);
    }
  }
}

// -- Helper: create a UDP multicast receiver socket --

/// Join a multicast group with logging.
/// @param iface  Interface IP to bind, or nullptr for INADDR_ANY.
/// @return true on success.
bool join_mcast_group(mk::net::UdpSocket &sock, const char *group,
                      const char *iface) {
  if (!sock.join_multicast_group(group, iface)) {
    mk::sys::log::signal_log("[PIPELINE] Multicast join failed for ", group,
                             '\n');
    return false;
  }
  return true;
}

/// Create a non-blocking UDP socket, bind to port, and join a multicast group.
/// @return UdpSocket on success, std::nullopt on failure.
std::optional<mk::net::UdpSocket> create_mcast_receiver(const char *group,
                                                        std::uint16_t port) {
  const int fd =
      ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    mk::sys::log::signal_log(
        "[PIPELINE] UDP socket() failed: ", strerror(errno), '\n');
    return std::nullopt;
  }
  mk::net::UdpSocket sock(fd);

  // Allow multiple processes to bind the same multicast port (for testing).
  (void)sock.set_reuseaddr(true);

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = INADDR_ANY;
  bind_addr.sin_port = htons(port);

  if (::bind(sock.get(), reinterpret_cast<sockaddr *>(&bind_addr),
             sizeof(bind_addr)) < 0) {
    mk::sys::log::signal_log("[PIPELINE] UDP bind() failed: ", strerror(errno),
                             '\n');
    return std::nullopt; // UdpSocket destructor closes fd.
  }

  if (!join_mcast_group(sock, group, nullptr)) {
    return std::nullopt;
  }

  // SO_BUSY_POLL: kernel polls NIC driver before sleeping in epoll_wait.
  // Reduces recv wakeup latency by ~5-20us. Best-effort — requires NAPI driver.
  (void)sock.set_busy_poll(50);

  return sock;
}

/// Print shutdown statistics (cold path — called once after threads join).
template <std::uint32_t MaxSymbols>
void print_shutdown_summary(
    const mk::app::FeedHandler &feed, const mk::app::OrderManager &om,
    const ConnectionState &conn, const MdFeedSource &feed_a,
    const MdFeedSource &feed_b, std::uint64_t signals_generated,
    bool feed_b_enabled, std::int64_t stats_ns) noexcept {
  mk::sys::log::signal_log("--- Pipeline Summary ---\n");
  mk::sys::log::signal_log("  Market data updates: ", feed.total_updates(),
                           '\n');
  mk::sys::log::signal_log("  Sequence gaps: ", feed.gap_count(), '\n');
  mk::sys::log::signal_log("  Duplicates (A/B): ", feed.duplicate_count(),
                           '\n');
  mk::sys::log::signal_log("  Signals generated: ", signals_generated, '\n');
  mk::sys::log::signal_log("  Orders sent: ", om.orders_sent(), '\n');
  mk::sys::log::signal_log("  Fills received: ", om.fills_received(), '\n');
  for (std::uint32_t s = 1; s <= MaxSymbols; ++s) {
    mk::sys::log::signal_log("  Position [sym ", s, "]: ", om.net_position(s),
                             '\n');
  }
  mk::sys::log::signal_log("  Modifies sent: ", om.modifies_sent(),
                           " acked: ", om.modifies_acked(), '\n');
  mk::sys::log::signal_log("  Rejects: ", om.rejects_received(), '\n');

  // Risk summary.
  mk::sys::log::signal_log("--- Risk Summary ---\n");
  mk::sys::log::signal_log(
      "  Rejects (outstanding): ", om.risk_rejects_outstanding(), '\n');
  mk::sys::log::signal_log(
      "  Rejects (position):    ", om.risk_rejects_position(), '\n');
  mk::sys::log::signal_log("  Rejects (order size):  ", om.risk_rejects_size(),
                           '\n');
  mk::sys::log::signal_log(
      "  Rejects (notional):    ", om.risk_rejects_notional(), '\n');
  mk::sys::log::signal_log("  Rejects (rate limit):  ", om.risk_rejects_rate(),
                           '\n');
  mk::sys::log::signal_log(
      "  Rejects (kill switch): ", om.risk_rejects_killswitch(), '\n');
  mk::sys::log::signal_log("  Order timeouts:        ", om.timeouts_fired(),
                           '\n');
  mk::sys::log::signal_log("  Kill switch state:     ",
                           static_cast<int>(om.kill_switch_state()), '\n');

  // Connection health summary.
  mk::sys::log::signal_log("--- Connection Health ---\n");
  mk::sys::log::signal_log(
      "  Heartbeats sent/recv:  ",
      conn.heartbeats_sent.load(std::memory_order_relaxed), "/",
      conn.heartbeats_recv.load(std::memory_order_relaxed), '\n');
  mk::sys::log::signal_log("  Reconnections:         ",
                           conn.reconnect_count.load(std::memory_order_relaxed),
                           '\n');
  mk::sys::log::signal_log(
      "  Send would-block:      ",
      conn.send_would_block.load(std::memory_order_relaxed), '\n');
  mk::sys::log::signal_log("  Send failures:         ",
                           conn.send_failures.load(std::memory_order_relaxed),
                           '\n');
  mk::sys::log::signal_log("  Feed B:                ",
                           feed_b_enabled ? "enabled" : "disabled", '\n');
  mk::sys::log::signal_log("  Stats dump interval:   ",
                           stats_ns > 0 ? stats_ns / 1'000'000'000 : 0, "s\n");

  // Per-feed statistics.
  mk::sys::log::signal_log("--- Feed Statistics ---\n");
  mk::sys::log::signal_log("  FeedA packets:         ", feed_a.stats.packets,
                           '\n');
  mk::sys::log::signal_log("  FeedA bytes:           ", feed_a.stats.bytes,
                           '\n');
  mk::sys::log::signal_log(
      "  FeedA drops:           ", feed_a.stats.datagrams_dropped, '\n');
  mk::sys::log::signal_log(
      "  FeedA queue drops:     ", feed_a.stats.queue_drops, '\n');
  if (feed_b_enabled) {
    mk::sys::log::signal_log("  FeedB packets:         ", feed_b.stats.packets,
                             '\n');
    mk::sys::log::signal_log("  FeedB bytes:           ", feed_b.stats.bytes,
                             '\n');
    mk::sys::log::signal_log(
        "  FeedB drops:           ", feed_b.stats.datagrams_dropped, '\n');
    mk::sys::log::signal_log(
        "  FeedB queue drops:     ", feed_b.stats.queue_drops, '\n');
  }
}

/// Resolve and validate NUMA topology for hot-path threads and memory policy.
/// Returns false on startup-gate violation (caller should exit).
[[nodiscard]] bool configure_numa_topology(std::int32_t pin_core_strategy,
                                           std::int32_t pin_core_md,
                                           bool strict_affinity,
                                           const std::string &nic_iface,
                                           int &numa_node) noexcept {
  // Industry-style policy for low-latency pipeline:
  //   MD core, Strategy core, NIC, and hot-path memory should be on one node.
  const int strategy_numa =
      (pin_core_strategy >= 0)
          ? mk::sys::thread::get_cpu_numa_node(
                static_cast<std::uint32_t>(pin_core_strategy))
          : -1;
  const int md_numa = (pin_core_md >= 0)
                          ? mk::sys::thread::get_cpu_numa_node(
                                static_cast<std::uint32_t>(pin_core_md))
                          : -1;

  const auto check_hot_thread_affinity =
      [&](const char *role, std::int32_t core, int numa,
          const char *flag_name) noexcept -> bool {
    if (core < 0) {
      if (strict_affinity) {
        mk::sys::log::signal_log("[PIPELINE] ERROR: ", role,
                                 " pinning disabled (", flag_name,
                                 "=-1) under --strict_affinity=true\n");
        return false;
      }
      mk::sys::log::signal_log("[PIPELINE] ", role, " pinning: disabled (",
                               flag_name, "=-1)\n");
      return true;
    }

    if (numa >= 0) {
      mk::sys::log::signal_log("[PIPELINE] ", role, " core ", core,
                               " -> NUMA node ", numa, '\n');
      return true;
    }

    if (strict_affinity) {
      mk::sys::log::signal_log("[PIPELINE] ERROR: ", role,
                               " core NUMA node unknown under "
                               "--strict_affinity=true\n");
      return false;
    }
    mk::sys::log::signal_log("[PIPELINE] WARNING: ", role,
                             " core NUMA node unknown\n");
    return true;
  };

  if (!check_hot_thread_affinity("Strategy", pin_core_strategy, strategy_numa,
                                 "--pin_core_strategy")) {
    return false;
  }
  if (!check_hot_thread_affinity("MD", pin_core_md, md_numa, "--pin_core_md")) {
    return false;
  }

  const auto read_nic_numa = [&]() noexcept -> int {
    if (nic_iface.empty()) {
      return -1;
    }
    const int numa = mk::sys::thread::get_nic_numa_node(nic_iface.c_str());
    if (numa >= 0) {
      mk::sys::log::signal_log("[PIPELINE] NIC ", nic_iface.c_str(),
                               " -> NUMA node ", numa, '\n');
    } else {
      mk::sys::log::signal_log("[PIPELINE] NIC ", nic_iface.c_str(),
                               " NUMA node: unknown (virtual NIC or sysfs "
                               "unavailable)\n");
    }
    return numa;
  };
  const int nic_numa = read_nic_numa();

  const auto check_node_match = [&](const char *lhs_name, int lhs_node,
                                    const char *rhs_name,
                                    int rhs_node) noexcept -> bool {
    if (lhs_node >= 0 && rhs_node >= 0 && lhs_node != rhs_node) {
      mk::sys::log::signal_log("[PIPELINE] ERROR: ", lhs_name, "=", lhs_node,
                               " mismatches ", rhs_name, "=", rhs_node, '\n');
      return false;
    }
    return true;
  };

  // Fail fast on explicit cross-node thread placement.
  if (!check_node_match("Strategy core node", strategy_numa, "MD core node",
                        md_numa)) {
    mk::sys::log::signal_log(
        "[PIPELINE] Keep latency-critical threads on the same NUMA node.\n");
    return false;
  }

  // Resolve allocation node.
  // numa_node semantics:
  //   -1: auto mode (prefer NIC node, then Strategy node, then MD node)
  //   >=0: force memory binding to that node
  const auto resolve_alloc_numa = [&](int requested_numa, int nic_node,
                                      int strategy_node,
                                      int md_node) noexcept -> int {
    if (requested_numa >= 0) {
      return requested_numa;
    }
    if (nic_node >= 0) {
      return nic_node;
    }
    if (strategy_node >= 0) {
      return strategy_node;
    }
    if (md_node >= 0) {
      return md_node;
    }
    return -1;
  };
  numa_node = resolve_alloc_numa(numa_node, nic_numa, strategy_numa, md_numa);

  // Final consistency checks when both sides are known.
  if (numa_node >= 0) {
    if (!check_node_match("numa_node", numa_node, "Strategy core node",
                          strategy_numa)) {
      return false;
    }
    if (!check_node_match("numa_node", numa_node, "MD core node", md_numa)) {
      return false;
    }
    if (!check_node_match("numa_node", numa_node, "NIC node", nic_numa)) {
      return false;
    }
    mk::sys::log::signal_log("[PIPELINE] Memory NUMA node: ", numa_node, '\n');
  } else {
    mk::sys::log::signal_log(
        "[PIPELINE] NUMA binding: disabled (node unresolved)\n");
  }

  return true;
}

} // namespace

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  // -- Read flags --
  // 1) External endpoints (exchange + market data feeds)
  const auto exchange_host = absl::GetFlag(FLAGS_exchange_host);
  const auto exchange_port = absl::GetFlag(FLAGS_exchange_port);
  const auto mcast_group = absl::GetFlag(FLAGS_mcast_group);
  const auto mcast_port = absl::GetFlag(FLAGS_mcast_port);
  const auto mcast_group_b = absl::GetFlag(FLAGS_mcast_group_b);
  const auto mcast_port_b = absl::GetFlag(FLAGS_mcast_port_b);

  // 2) Strategy knobs
  const auto spread_threshold = absl::GetFlag(FLAGS_spread_threshold);
  const auto order_qty = absl::GetFlag(FLAGS_order_qty);

  // 3) Risk limits / OMS behavior
  const auto max_outstanding = absl::GetFlag(FLAGS_max_outstanding);
  const auto max_outstanding_per_symbol =
      absl::GetFlag(FLAGS_max_outstanding_per_symbol);
  const auto max_position = absl::GetFlag(FLAGS_max_position);
  const auto max_order_size = absl::GetFlag(FLAGS_max_order_size);
  const auto max_notional = absl::GetFlag(FLAGS_max_notional);
  const auto max_orders_per_sec = absl::GetFlag(FLAGS_max_orders_per_sec);
  const auto order_timeout_ms = absl::GetFlag(FLAGS_order_timeout_ms);

  // 4) Thread placement / NUMA policy
  const auto pin_core_md = absl::GetFlag(FLAGS_pin_core_md);
  const auto pin_core_strategy = absl::GetFlag(FLAGS_pin_core_strategy);
  const auto pin_core_logger = absl::GetFlag(FLAGS_pin_core_logger);
  const auto strict_affinity = absl::GetFlag(FLAGS_strict_affinity);
  auto numa_node = absl::GetFlag(FLAGS_numa_node); // mutable: auto-detect
  const auto nic_iface = absl::GetFlag(FLAGS_nic_iface);

  // 5) Runtime buffer sizing
  const std::size_t tcp_rx_sz = absl::GetFlag(FLAGS_tcp_rx_size);
  const std::size_t tcp_tx_sz = absl::GetFlag(FLAGS_tcp_tx_size);
  const std::size_t scratch_sz = absl::GetFlag(FLAGS_scratch_size);

  mk::sys::log::signal_log("[PIPELINE] Starting tick-to-trade pipeline\n");

  // -- Cold path: initialization --

  // Force link of global new/delete guard.
  mk::sys::memory::install_global_memory_guard();

  // Verify TSC reliability (invariant TSC required for RDTSC measurements).
  mk::sys::assert_tsc_reliable();

  // Calibrate TSC for cycle-to-nanosecond conversion.
  mk::sys::log::signal_log("[PIPELINE] Calibrating TSC...\n");
  auto tsc_cal = mk::sys::TscCalibration::calibrate();
  mk::sys::log::signal_log("[PIPELINE] TSC frequency: ");
  {
    char freq_buf[32];
    std::snprintf(freq_buf, sizeof(freq_buf), "%.3f", tsc_cal.freq_ghz());
    mk::sys::log::signal_log(freq_buf);
  }
  mk::sys::log::signal_log(" GHz\n");

  // -- NUMA topology check (startup gate) --
  if (!configure_numa_topology(pin_core_strategy, pin_core_md, strict_affinity,
                               nic_iface, numa_node)) {
    return 1;
  }

  // -- Create UDP multicast receivers --
  // Feed A: always required.
  auto udp_opt = create_mcast_receiver(mcast_group.c_str(), mcast_port);
  if (!udp_opt) {
    return 1;
  }
  mk::net::UdpSocket udp_sock(std::move(*udp_opt));

  // Feed B: separate socket for true A/B redundancy (optional).
  // Each feed gets an independent kernel receive buffer, enabling
  // per-NIC binding and fault isolation. FeedHandler deduplicates by seq_num.
  std::optional<mk::net::UdpSocket> udp_sock_b;
  if (!mcast_group_b.empty()) {
    auto udp_b_opt = create_mcast_receiver(mcast_group_b.c_str(), mcast_port_b);
    if (!udp_b_opt) {
      return 1;
    }
    udp_sock_b.emplace(std::move(*udp_b_opt));
    mk::sys::log::signal_log("[PIPELINE] Feed B socket: ",
                             mcast_group_b.c_str(), ":", mcast_port_b, '\n');
  }
  // Raw fd for event loop dispatch (-1 = no Feed B).
  const int udp_b_fd = udp_sock_b ? udp_sock_b->get() : -1;

  // -- Connect to exchange TCP gateway --
  mk::sys::log::signal_log("[PIPELINE] Connecting to exchange ",
                           exchange_host.c_str(), ":", exchange_port, "...\n");
  auto tcp_opt = connect_to_exchange(exchange_host.c_str(), exchange_port);
  if (!tcp_opt) {
    return 1; // udp_sock closed by RAII
  }
  mk::sys::log::signal_log("[PIPELINE] Connected to exchange\n");
  mk::net::TcpSocket tcp_sock(std::move(*tcp_opt));

  // -- Allocate all hot-path memory (cold path, startup only) --
  //
  // Every MmapRegion uses two-level huge page fallback:
  //   1. Try 2MB huge pages (MAP_HUGETLB) — best TLB efficiency.
  //   2. Fall back to anonymous mmap + MADV_HUGEPAGE hint (THP).
  //   3. NUMA bind + deferred prefault when numa_node >= 0.
  //   4. Abort on failure (unrecoverable at startup).
  //
  // Non-owning allocation pattern: main allocates MmapRegions, components
  // receive raw pointers.  MdToStrategyQueue / SPSCQueue still allocate
  // internally.
  using namespace mk::sys::memory;
  // Startup allocation policy for this app:
  // - kPopulateWrite: prefault writable pages now.
  // - lock_pages=false: we use process-wide mlockall() after all allocations.
  //   This keeps policy in one place and avoids per-region lock flag noise.
  const RegionIntentConfig startup_alloc_policy{
      .numa_node = numa_node,
      .lock_pages = false,
      .page_mapping_mode = PageMappingMode::kExplicitThenThp,
      .huge_page_size = HugePageSize::k2MB,
      .failure_mode = FailureMode::kFailFast,
  };
  auto alloc_startup_region = [&](std::size_t bytes) {
    auto cfg = startup_alloc_policy;
    cfg.size = bytes;
    return allocate_hot_rw_region(cfg);
  };

  // StrategyCtx: single contiguous region for the strategy thread —
  // TCP I/O buffers + OMS state (positions, active orders, outstanding map,
  // cancel buffer, timeout wheel).
  // Timeout wheel: 1ms per tick, wheel_size = round_up_pow2(timeout_ticks * 2).
  const auto timeout_ticks = static_cast<std::size_t>(order_timeout_ms);
  const auto timeout_wheel_size =
      mk::ds::TimingWheel::round_up_capacity(timeout_ticks * 2);
  const auto timeout_max_timers =
      mk::ds::TimingWheel::round_up_capacity(max_outstanding);
  const auto strat_buf_total = mk::app::strategy_ctx_buf_size<ActiveStrategy>(
      tcp_rx_sz, tcp_tx_sz, scratch_sz, max_outstanding, timeout_wheel_size,
      timeout_max_timers);
  auto strat_buf_region = alloc_startup_region(strat_buf_total);
  mk::sys::log::signal_log("[PIPELINE] StrategyCtx: rx=", tcp_rx_sz,
                           " tx=", tcp_tx_sz, " scratch=", scratch_sz,
                           " symbols=", ActiveStrategy::kMaxSymbols,
                           " outstanding=", max_outstanding,
                           " per_symbol=", max_outstanding_per_symbol, " (",
                           strat_buf_region.size(), " bytes)\n");

  // MdCtx: MD thread recvmmsg scatter-gather arrays (non-owning).
  // Batch size 64: power-of-two, mmsghdr array fits in L1 (~3.5KB),
  // amortizes syscall overhead across UDP burst. Matches kDrainBatch
  // in strategy_thread.hpp so consumer keeps pace with producer.
  constexpr unsigned int kMdBatchSize = 64;
  constexpr std::size_t kMdBufSize = 64; // per-datagram buffer (36B wire + pad)
  auto md_region =
      alloc_startup_region(mk::app::md_ctx_buf_size(kMdBatchSize, kMdBufSize));
  mk::sys::log::signal_log("[PIPELINE] MdCtx: batch=", kMdBatchSize,
                           " buf=", kMdBufSize, " (", md_region.size(),
                           " bytes)\n");

  // MdToStrategyQueue: MD thread → Strategy thread SPSC queue buffer.
  constexpr std::uint32_t kMdToStrategyQueueCapacity = 1024;
  auto queue_region =
      alloc_startup_region(kMdToStrategyQueueCapacity * sizeof(QueuedUpdate));
  mk::sys::log::signal_log(
      "[PIPELINE] MdToStrategyQueue: capacity=", kMdToStrategyQueueCapacity,
      " (", queue_region.size(), " bytes)\n");

  // -- Carve pre-allocated regions into typed contexts --
  auto strat_ctx = mk::app::make_strategy_ctx<ActiveStrategy>(
      static_cast<std::byte *>(strat_buf_region.get()), tcp_rx_sz, tcp_tx_sz,
      scratch_sz, max_outstanding, max_outstanding_per_symbol,
      timeout_wheel_size, timeout_max_timers);
  auto md_ctx = mk::app::make_md_ctx(md_region.get(), kMdBatchSize, kMdBufSize);

  // -- MD thread components --
  mk::app::FeedHandler feed_handler;
  mk::sys::log::signal_log("[PIPELINE] FeedHandler ready\n");

  // -- Strategy thread components --
  ActiveStrategy strategy(spread_threshold, order_qty);
  mk::sys::log::signal_log("[PIPELINE] SpreadStrategy: threshold=",
                           spread_threshold, " qty=", order_qty, "\n");
  const auto order_timeout_ns =
      static_cast<std::int64_t>(order_timeout_ms) * 1'000'000;
  mk::app::OrderManager order_mgr(strat_ctx, max_position, max_order_size,
                                  max_notional, max_orders_per_sec,
                                  1'000'000'000, order_timeout_ns);
  mk::sys::log::signal_log(
      "[PIPELINE] OrderManager: max_pos=", max_position,
      " max_size=", max_order_size, " max_notional=", max_notional,
      " rate=", max_orders_per_sec, "/s timeout=", order_timeout_ms, "ms\n");
  // -- Shared / infrastructure --

  // LatencyTracker: mmap-backed (~66KB) for consistent page residency.
  // Shared by MD thread (feed parse) and Strategy thread (all other stages).
  auto tracker_region = alloc_startup_region(sizeof(mk::app::LatencyTracker));
  auto *tracker = std::construct_at(
      static_cast<mk::app::LatencyTracker *>(tracker_region.get()));
  mk::sys::log::signal_log("[PIPELINE] LatencyTracker: ", tracker_region.size(),
                           " bytes\n");
  MdToStrategyQueue md_queue(queue_region.get(), queue_region.size(),
                             kMdToStrategyQueueCapacity);

  // Log queue buffers — centralized allocation, same NUMA node as
  // hot-path producers (MD, Strategy). Queues are non-owning SPSCQueues;
  // the drain loop only receives pointers via register_queue().
  constexpr std::uint32_t kLogQueueCapacity = 4096;
  auto md_log_region = alloc_startup_region(
      mk::app::PipelineLogQueue::required_buffer_size(kLogQueueCapacity));
  auto strat_log_region = alloc_startup_region(
      mk::app::PipelineLogQueue::required_buffer_size(kLogQueueCapacity));
  mk::sys::log::signal_log(
      "[PIPELINE] Log queues: capacity=", kLogQueueCapacity, " (",
      md_log_region.size() + strat_log_region.size(), " bytes)\n");

  mk::app::PipelineLogQueue md_log_queue(
      md_log_region.get(), md_log_region.size(), kLogQueueCapacity);
  mk::app::PipelineLogQueue strat_log_queue(
      strat_log_region.get(), strat_log_region.size(), kLogQueueCapacity);

  using DrainLoop = mk::sys::log::AsyncDrainLoop<mk::app::LogEntry,
                                                 mk::app::PipelineLogFormatter>;
  DrainLoop drain_loop("pipeline.log");
  drain_loop.register_queue(&md_log_queue);
  drain_loop.register_queue(&strat_log_queue);
  drain_loop.start(pin_core_logger);
  mk::sys::log::signal_log("[PIPELINE] DrainLoop: core=", pin_core_logger,
                           "\n");

  // -- Lock all pages into RAM --
  // Prevents swap-induced major page faults during trading.
  // MCL_CURRENT: lock all currently mapped pages (prefaulted hot-path buffers).
  // MCL_FUTURE:  auto-lock future allocations (thread stacks, etc.).
  // Non-fatal: trading still works without mlock, just with swap risk.
  // Requires RLIMIT_MEMLOCK to be raised (ulimit -l unlimited).
  if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    mk::sys::log::signal_log(
        "[PIPELINE] WARNING: mlockall failed (errno=", errno,
        "). Pages may be swapped out. Check RLIMIT_MEMLOCK (ulimit -l).\n");
  } else {
    mk::sys::log::signal_log("[PIPELINE] mlockall: all pages locked\n");
  }

  // -- Signal handling --
  // SIGINT/SIGTERM: graceful shutdown (first press = kill switch, second =
  // force) SIGUSR1: explicit kill switch trigger (e.g., from monitoring system)
  struct sigaction sa{};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGUSR1, &sa, nullptr);

  // -- Initialize connection state --
  ConnectionState conn;
  {
    const auto now = mk::sys::monotonic_nanos();
    conn.last_hb_sent = now;
    conn.last_hb_recv = now;
  }

  const auto stats_ns = absl::GetFlag(FLAGS_stats_interval) * 1'000'000'000LL;

  // -- Set up per-thread epoll instances --

  // MD thread: epoll for UDP-only events.
  // MdFeedSource structs live here (main scope) — their lifetime spans
  // the MD thread. epoll stores pointers to these in data.ptr.
  MdFeedSource feed_a{.fd = udp_sock.get(), .slot = MdEpollSlot::kIncrementalA};
  MdFeedSource feed_b{.fd = udp_b_fd, .slot = MdEpollSlot::kIncrementalB};

  mk::net::EpollWrapper md_epoll;
  md_epoll.add(feed_a.fd, EPOLLIN | EPOLLET, &feed_a);
  if (udp_b_fd >= 0) {
    md_epoll.add(feed_b.fd, EPOLLIN | EPOLLET, &feed_b);
  }

  // Strategy thread: epoll for TCP-only events.
  mk::net::EpollWrapper strategy_epoll;
  strategy_epoll.add(tcp_sock.get(), EPOLLIN | EPOLLET, &conn);

  // -- Spawn MD thread + Strategy thread --

  mk::sys::log::signal_log("[PIPELINE] Spawning MD thread + Strategy thread\n");

  mk::app::MdFeedThread md_thread({
      .epoll = md_epoll,
      .feed_handler = feed_handler,
      .tracker = *tracker,
      .md_ctx = md_ctx,
      .md_queue = md_queue,
      .log_queue = md_log_queue,
      .stop_flag = g_stop,
      .pin_core_md = pin_core_md,
      .max_symbols = ActiveStrategy::kMaxSymbols,
  });
  md_thread.start();

  mk::app::StrategyThread<ActiveStrategy> strategy_thread({
      .md_queue = md_queue,
      .strategy = strategy,
      .order_mgr = order_mgr,
      .tcp_sock = tcp_sock,
      .epoll = strategy_epoll,
      .conn = conn,
      .tcp_tx_buf = strat_ctx.tcp_tx,
      .scratch = strat_ctx.scratch,
      .tcp_rx_data = strat_ctx.tcp_rx,
      .tcp_rx_size = strat_ctx.tcp_rx_size,
      .stop_flag = g_stop,
      .kill_switch_flag = g_kill_switch,
      .tracker = *tracker,
      .log_queue = strat_log_queue,
      .exchange_host = exchange_host.c_str(),
      .exchange_port = exchange_port,
      .pin_core = pin_core_strategy,
      .stats_interval_ns = stats_ns,
  });
  strategy_thread.start();

  strategy_thread.join();
  // Strategy thread exited. Signal MD thread to stop.
  g_stop.test_and_set(std::memory_order_relaxed);
  md_thread.join();

  // Stop async logger after both hot threads have exited (no more pushes).
  // Final drain flushes remaining entries to file.
  drain_loop.stop();
  mk::sys::log::signal_log("[PIPELINE] Async logger stopped (",
                           drain_loop.entries_written(), " entries written)\n");

  // ================================================================
  // COLD PATH: Shutdown
  // ================================================================

  mk::sys::log::signal_log("\n[PIPELINE] Shutting down...\n");

  // Warn if forced exit left orders in-flight.
  if (order_mgr.outstanding_count() > 0 &&
      order_mgr.kill_switch_state() !=
          mk::app::OrderManager::KillSwitchState::kComplete) {
    mk::sys::log::signal_log(
        "[PIPELINE] WARNING: ", order_mgr.outstanding_count(),
        " orders still in-flight (forced exit before cancel-all completed)\n");
  }

  // Print latency statistics.
  tracker->print_stats(tsc_cal);

  // Print pipeline, risk, and connection health summary.
  print_shutdown_summary<ActiveStrategy::kMaxSymbols>(
      feed_handler, order_mgr, conn, feed_a, feed_b,
      strategy.signals_generated(), udp_sock_b.has_value(), stats_ns);

  // Cleanup: leave multicast, graceful TCP close, then RAII destructors.

  // Leave multicast groups before closing UDP sockets.
  {
    in_addr iface{};
    iface.s_addr = htonl(INADDR_ANY);

    // Feed A
    in_addr group{};
    inet_pton(AF_INET, mcast_group.c_str(), &group);
    (void)udp_sock.leave_multicast_group(group, iface);
    md_epoll.remove(udp_sock.get());

    // Feed B (independent socket)
    if (udp_sock_b) {
      in_addr group_b{};
      inet_pton(AF_INET, mcast_group_b.c_str(), &group_b);
      (void)udp_sock_b->leave_multicast_group(group_b, iface);
      md_epoll.remove(udp_sock_b->get());
    }
  }

  // Graceful TCP close: send FIN before closing socket.
  if (tcp_sock.is_valid()) {
    (void)tcp_sock.shutdown(SHUT_WR);
    strategy_epoll.remove(tcp_sock.get());
  }
  // Close connecting socket if reconnection was in progress.
  // ScopedFd ensures close even if early return were added later.
  const mk::net::ScopedFd connecting_guard(conn.connecting_fd);
  conn.connecting_fd = -1;
  // udp_sock, tcp_sock, connecting_guard closed by RAII (destructors).

  // Destroy placement-new'd objects before their backing regions are freed.
  std::destroy_at(tracker);

  mk::sys::log::signal_log("[PIPELINE] Shutdown complete.\n");

  return 0;
}
