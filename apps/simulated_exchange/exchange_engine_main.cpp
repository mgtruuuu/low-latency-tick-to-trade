/**
 * @file exchange_engine_main.cpp
 * @brief Exchange engine process — pure matching engine with multi-gateway IPC.
 *
 * This is the central process of the multi-gateway simulated exchange.
 * It has NO network I/O — only shared memory queues:
 *
 *   request_queues[0..N-1]  ──► ExchangeCore ──► response_queues[0..N-1]
 *                                             ──► md_event_queue
 *
 * The engine round-robin polls N request queues (one per gateway) and
 * routes responses to the correct gateway via a session_to_gateway mapping.
 * This ensures cross-gateway fills are delivered to the right client.
 *
 * This process creates and owns the shared memory region. It must start
 * before the Gateway and Publisher processes.
 *
 * Usage:
 *   ./exchange_engine --num_gateways=2 --shm_name=/mk_exchange_events
 */

#include "exchange_core.hpp"
#include "shared_exchange_queue.hpp"

#include "sys/log/signal_logger.hpp"
#include "sys/memory/mmap_region.hpp"
#include "sys/thread/affinity.hpp"

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>

// -- Command-line flags --

ABSL_FLAG(std::string, shm_name, std::string(mk::app::kDefaultShmName),
          "POSIX shared memory name for IPC");
ABSL_FLAG(uint32_t, num_gateways, 1,
          "Number of gateway processes to poll (max kMaxGateways)");
ABSL_FLAG(uint32_t, symbol_count, 2, "Number of symbols (max 2)");
ABSL_FLAG(int32_t, pin_core, -1,
          "CPU core to pin the main loop (-1 = no pinning)");

// -- Global stop flag (signal-safe) --

namespace {

std::atomic_flag g_stop = ATOMIC_FLAG_INIT;

void signal_handler(int /*signum*/) {
  g_stop.test_and_set(std::memory_order_relaxed);
}

/// Drain ExchangeCore events and route to the correct gateway's response
/// queue + md_event_queue.
///
/// Cross-gateway fill routing: events are routed by
/// session_to_gateway[event.session_id]. ExchangeCore emits fill events
/// for BOTH taker and maker sessions, so when Client A (gateway 0) fills
/// against Client B's (gateway 1) resting order, both gateways receive
/// their respective FillReports via the correct response queues.
///
/// Known tradeoff (simulator scope): on push failure, events are dropped
/// with a log warning. Production would use backpressure or explicit reject.
void drain_and_dispatch(
    mk::app::ExchangeCore<> &core, std::uint32_t request_seq,
    mk::app::SharedEventQueue *response_queues,
    const std::array<std::uint8_t, 65536> &session_to_gateway,
    mk::app::SharedEventQueue &md_queue) noexcept {
  for (const auto &event : core.drain_events()) {
    auto stamped = event;
    stamped.request_seq = request_seq;

    // Route by event type:
    //   kFill       → response_queue (TCP) + md_queue (UDP trade)
    //   kBBOUpdate  → md_queue only (UDP BBO, gateway doesn't serialize it)
    //   everything else (Ack, Reject, etc.) → response_queue only
    switch (event.type) {
    case mk::app::EventType::kFill: {
      const auto gw = session_to_gateway[event.session_id];
      if (!response_queues[gw].try_push(stamped)) [[unlikely]] {
        mk::sys::log::signal_log("[ENGINE] Response queue full (gw=", gw,
                                 "), event dropped\n");
      }
      if (!md_queue.try_push(stamped)) [[unlikely]] {
        mk::sys::log::signal_log("[ENGINE] MD queue full, event dropped\n");
      }
      break;
    }
    case mk::app::EventType::kBBOUpdate:
      if (!md_queue.try_push(stamped)) [[unlikely]] {
        mk::sys::log::signal_log("[ENGINE] MD queue full, event dropped\n");
      }
      break;
    default: {
      const auto gw = session_to_gateway[event.session_id];
      if (!response_queues[gw].try_push(stamped)) [[unlikely]] {
        mk::sys::log::signal_log("[ENGINE] Response queue full (gw=", gw,
                                 "), event dropped\n");
      }
      break;
    }
    }
  }
}

/// Process one OrderRequest from a gateway.
void process_request(const mk::app::OrderRequest &req,
                     mk::app::ExchangeCore<> &core,
                     mk::app::SharedEventQueue *response_queues,
                     std::uint32_t num_gateways,
                     std::array<std::uint8_t, 65536> &session_to_gateway,
                     mk::app::SharedEventQueue &md_queue) noexcept {

  // Reject requests from gateways outside the configured range.
  if (req.gateway_id >= num_gateways) [[unlikely]] {
    mk::sys::log::signal_log(
        "[ENGINE] Request from unconfigured gateway_id=", req.gateway_id,
        " (num_gateways=", num_gateways, "), dropped\n");
    return;
  }

  using mk::app::EventType;
  using mk::app::ExchangeEvent;
  using mk::app::OrderRequestType;

  switch (req.type) {
  case OrderRequestType::kClientConnect: {
    const auto session_id = core.on_client_connect();
    // Record which gateway owns this session (for cross-gateway fill routing).
    session_to_gateway[session_id] = req.gateway_id;
    // Echo client_order_id (contains fd) so send thread can map session → fd.
    if (!response_queues[req.gateway_id].try_push(ExchangeEvent{
            .type = EventType::kSessionAssigned,
            .session_id = session_id,
            .request_seq = req.request_seq,
            .client_order_id = req.client_order_id})) [[unlikely]] {
      // Roll back the partially-created session. The gateway never receives
      // kSessionAssigned, so from its point of view the session does not exist.
      // Without rollback, the engine would retain ghost state:
      //   - core.active_sessions_[session_id] stays true (session ID leak)
      //   - session_to_gateway[session_id] remains set (stale routing)
      core.on_client_disconnect(session_id);
      session_to_gateway[session_id] = 0;
      mk::sys::log::signal_log(
          "[ENGINE] Response queue full on session assignment, rolled back "
          "session=",
          session_id, '\n');
    }
    break;
  }

  case OrderRequestType::kClientDisconnect: {
    core.on_client_disconnect(req.session_id);
    break;
  }

  case OrderRequestType::kNewOrder: {
    const mk::app::NewOrder order{
        .client_order_id = req.client_order_id,
        .symbol_id = req.symbol_id,
        .side = req.side,
        .price = req.price,
        .qty = req.qty,
        .send_ts = req.send_ts,
    };
    core.submit_order(req.session_id, order);
    drain_and_dispatch(core, req.request_seq, response_queues,
                       session_to_gateway, md_queue);
    break;
  }

  case OrderRequestType::kCancelOrder: {
    const mk::app::CancelOrder cancel{
        .client_order_id = req.client_order_id,
        .symbol_id = req.symbol_id,
        .send_ts = req.send_ts,
    };
    core.cancel_order(req.session_id, cancel);
    drain_and_dispatch(core, req.request_seq, response_queues,
                       session_to_gateway, md_queue);
    break;
  }

  case OrderRequestType::kModifyOrder: {
    const mk::app::ModifyOrder modify{
        .client_order_id = req.client_order_id,
        .symbol_id = req.symbol_id,
        .new_price = req.new_price,
        .new_qty = req.new_qty,
        .send_ts = req.send_ts,
    };
    core.modify_order(req.session_id, modify);
    drain_and_dispatch(core, req.request_seq, response_queues,
                       session_to_gateway, md_queue);
    break;
  }
  }
}

} // namespace

// ======================================================================
// main
// ======================================================================

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  const auto shm_name = absl::GetFlag(FLAGS_shm_name);
  auto symbol_count = absl::GetFlag(FLAGS_symbol_count);
  symbol_count = std::clamp(symbol_count, 1U, 2U);
  auto num_gateways = absl::GetFlag(FLAGS_num_gateways);
  num_gateways = std::clamp(num_gateways, 1U, mk::app::kMaxGateways);

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
      mk::sys::log::signal_log("[ENGINE] Pinned to core ", pin_core, '\n');
    } else {
      mk::sys::log::signal_log("[ENGINE] Failed to pin to core ", pin_core,
                               " (errno=", err, ")\n");
    }
  }

  // -- Create shared memory region --
  const auto shm_size = sizeof(mk::app::SharedExchangeRegion);
  auto shm_region = mk::sys::memory::MmapRegion::open_shared(
      shm_name, shm_size, mk::sys::memory::ShmMode::kCreateOrOpen,
      mk::sys::memory::PrefaultPolicy::kPopulateWrite);
  if (!shm_region) {
    mk::sys::log::signal_log(
        "[ENGINE] Failed to create shared memory: ", shm_name.c_str(), '\n');
    return 1;
  }

  auto *shared = new (shm_region->data())
      mk::app::SharedExchangeRegion{}; // NOLINT(*-owning-memory)

  mk::sys::log::signal_log("[ENGINE] Shared memory created: ", shm_name.c_str(),
                           " size=", shm_size, " bytes\n");

  // -- Allocate per-symbol OrderBook memory (centralized, startup-only) --
  using Engine = mk::algo::MatchingEngine<>;
  const mk::algo::OrderBook::Params book_params{};
  const auto book_buf_size = Engine::required_buffer_size(book_params);

  auto book_region_1 = mk::sys::memory::allocate_hot_rw_region(
      mk::sys::memory::RegionIntentConfig{.size = book_buf_size});
  mk::sys::memory::MmapRegion book_region_2;
  if (symbol_count >= 2) {
    book_region_2 = mk::sys::memory::allocate_hot_rw_region(
        mk::sys::memory::RegionIntentConfig{.size = book_buf_size});
  }

  // -- Create exchange core --
  mk::app::ExchangeCore<> core;
  core.register_symbol(1, book_region_1.data(), book_region_1.size(),
                       book_params);
  if (symbol_count >= 2) {
    core.register_symbol(2, book_region_2.data(), book_region_2.size(),
                         book_params);
  }

  // Session-to-gateway routing table (engine-local, not in shared memory).
  // Indexed by session_id (uint16_t), value = gateway_id.
  // Used for cross-gateway fill routing: when order A (gateway 0) fills
  // against resting order B (gateway 1), the fill event for B must go
  // to gateway 1's response queue.
  std::array<std::uint8_t, 65536> session_to_gateway{};

  // Publish num_gateways and symbol_count so other processes can read them.
  shared->num_gateways.store(num_gateways, std::memory_order_relaxed);
  shared->symbol_count.store(symbol_count, std::memory_order_relaxed);

  // Signal readiness.
  shared->engine_ready.store(1U, std::memory_order_release);

  mk::sys::log::signal_log("[ENGINE] Ready. symbols=", symbol_count,
                           " gateways=", num_gateways,
                           " shm=", shm_name.c_str(), '\n');

  // -- Main event loop: round-robin poll all gateway request queues --
  std::uint64_t requests_processed = 0;
  constexpr std::uint64_t kLogInterval = 5000;

  while (!g_stop.test(std::memory_order_relaxed) &&
         shared->shutdown.load(std::memory_order_relaxed) == 0) {

    // Round-robin: poll each gateway's request queue once per iteration.
    for (std::uint32_t gw = 0; gw < num_gateways; ++gw) {
      mk::app::OrderRequest req{};
      if (!shared->request_queues[gw].try_pop(req)) {
        continue;
      }

      process_request(req, core, shared->response_queues, num_gateways,
                      session_to_gateway, shared->md_event_queue);
      ++requests_processed;

      if (requests_processed % kLogInterval == 0) {
        mk::sys::log::signal_log(
            "[ENGINE] requests=", requests_processed,
            " book1=", core.engine(1).book().total_orders(), '\n');
      }
    }
  }

  // -- Shutdown --
  shared->shutdown.store(1U, std::memory_order_release);
  ::shm_unlink(shm_name.c_str());

  mk::sys::log::signal_log("[ENGINE] Shutdown. requests=", requests_processed,
                           '\n');
  return 0;
}
