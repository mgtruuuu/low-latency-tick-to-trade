/**
 * @file strategy_ctx.hpp
 * @brief Strategy thread context — single contiguous buffer for all
 *        strategy-thread state (TCP I/O buffers + OrderManager OMS state).
 *
 * One context per thread: this struct holds every pointer the strategy
 * thread needs, carved from a single caller-provided MmapRegion. No logic
 * — just typed views over the raw buffer.
 *
 * Layout (single contiguous region):
 *   [tcp_rx:              tcp_rx_size bytes,  cache-line aligned]
 *   [tcp_tx:              tcp_tx_size bytes,  cache-line aligned]
 *   [scratch:             scratch_size bytes]
 *   --- align to max_align_t ---
 *   [net_position:            max_symbols * sizeof(int64_t)]
 *   [outstanding_per_symbol:  max_symbols * sizeof(uint32_t)]
 *   [active_orders:           max_symbols * kSideCount * sizeof(ActiveOrder)]
 *   [cancel_buf:          max_outstanding * sizeof(CancelOrder)]
 *   [map_slots:           map_capacity * OutstandingMap::slot_size()]
 *   [timeout_ids:         max_outstanding * sizeof(uint64_t)]
 *   [timeout_wheel_buf:   TimingWheel::required_buffer_size(ws, mt)]
 *
 * UDP recv buffers (recvmmsg scatter-gather arrays) are handled separately
 * by MdCtx on the MD feed thread.
 */

#pragma once

#include "order_types.hpp"    // OrderInfo, ActiveOrder, OutstandingMap, order_ctx_map_capacity
#include "strategy_policy.hpp" // StrategyPolicy concept (for buf_size/make templates)

#include "shared/protocol.hpp" // CancelOrder

#include "sys/bit_utils.hpp"
#include "sys/hardware_constants.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace mk::app {

/// Non-owning typed views into a contiguous buffer region for the strategy
/// thread. Caller is responsible for allocation, NUMA binding, and lifetime.
struct StrategyCtx {
  // -- TCP I/O buffers --

  char *tcp_rx;
  std::size_t tcp_rx_size;
  std::span<std::byte> tcp_tx;
  /// Scratch buffer for order serialization before TLV framing.
  /// Must be >= the largest message wire size in the protocol
  /// (currently kFillReportWireSize = 40 bytes).
  std::span<std::byte> scratch;

  // -- OMS state (OrderManager) --

  /// Number of sides per symbol (Bid + Ask). Derived from algo::Side::kCount.
  static constexpr std::size_t kSideCount =
      static_cast<std::size_t>(algo::Side::kCount);

  /// Per-symbol net position — [max_symbols] elements.
  std::int64_t *net_position;

  /// Per-symbol outstanding order count — [max_symbols] elements.
  /// Incremented on order send, decremented on fill/reject/cancel/timeout.
  std::uint32_t *outstanding_per_symbol;

  /// Per-(symbol, side) active resting orders — [max_symbols * kSideCount].
  /// Indexed as: active_orders[sym_idx * kSideCount + side_idx].
  ActiveOrder *active_orders;

  /// HashMap backing buffer for outstanding order tracking.
  /// OrderManager constructs OutstandingMap from these fields.
  void *map_buf;
  std::size_t map_buf_size;
  std::size_t map_capacity;

  /// Cancel buffer for kill switch — [max_outstanding] elements.
  /// trigger_kill_switch() writes CancelOrder messages into this buffer.
  CancelOrder *cancel_buf;

  /// TimingWheel backing buffer — OrderManager constructs TimingWheel from these.
  void *timeout_wheel_buf;
  std::size_t timeout_wheel_buf_size;
  std::size_t timeout_wheel_size;  // wheel_size param for TimingWheel constructor
  std::size_t timeout_max_timers;  // max_timers param for TimingWheel constructor

  /// Timeout batch buffer — [max_outstanding] elements.
  /// Timeout callback writes client_order_ids here during tick().
  std::uint64_t *timeout_ids;

  /// Runtime dimensions.
  std::uint32_t max_symbols;
  std::uint32_t max_outstanding;
  std::uint32_t max_outstanding_per_symbol;
};

// ==========================================================================
// Free functions
// ==========================================================================

/// Compute total MmapRegion size needed for the strategy thread context.
/// All sub-regions are aligned for their respective types.
/// Cache-line aligned between TCP regions to avoid false sharing.
template <StrategyPolicy Strategy>
[[nodiscard]] constexpr std::size_t
strategy_ctx_buf_size(std::size_t tcp_rx_size,
                      std::size_t tcp_tx_size,
                      std::size_t scratch_size,
                      std::uint32_t max_outstanding,
                      std::size_t timeout_wheel_size,
                      std::size_t timeout_max_timers) noexcept {
  constexpr std::uint32_t kMaxSymbols = Strategy::kMaxSymbols;

  // TCP I/O portion
  std::size_t offset = sys::align_up(tcp_rx_size, sys::kCacheLineSize) +
                       sys::align_up(tcp_tx_size, sys::kCacheLineSize) +
                       scratch_size;

  // OMS state portion — aligned to max_align_t boundary after TCP scratch.
  offset = sys::align_up(offset, alignof(std::max_align_t));

  // net_position: int64_t[kMaxSymbols]
  offset += kMaxSymbols * sizeof(std::int64_t);

  // outstanding_per_symbol: uint32_t[kMaxSymbols]
  offset = sys::align_up(offset, alignof(std::uint32_t));
  offset += kMaxSymbols * sizeof(std::uint32_t);

  // active_orders: ActiveOrder[kMaxSymbols * kSideCount]
  offset = sys::align_up(offset, alignof(ActiveOrder));
  offset += kMaxSymbols * StrategyCtx::kSideCount * sizeof(ActiveOrder);

  // cancel_buf: CancelOrder[max_outstanding]
  offset = sys::align_up(offset, alignof(CancelOrder));
  offset += max_outstanding * sizeof(CancelOrder);

  // map_slots: OutstandingMap slots
  const auto map_cap = order_ctx_map_capacity(max_outstanding);
  offset = sys::align_up(offset, OutstandingMap::slot_alignment());
  offset += OutstandingMap::required_buffer_size(map_cap);

  // timeout_ids: uint64_t[max_outstanding]
  offset = sys::align_up(offset, alignof(std::uint64_t));
  offset += max_outstanding * sizeof(std::uint64_t);

  // timeout_wheel_buf: TimingWheel backing buffer
  // alignof(max_align_t) = 16 on x86-64, satisfies TimerNode alignment (8).
  offset = sys::align_up(offset, alignof(std::max_align_t));
  offset += ds::TimingWheel::required_buffer_size(timeout_wheel_size,
                                                  timeout_max_timers);

  return offset;
}

/// Carve a contiguous buffer into typed strategy thread regions.
/// @tparam Strategy          Concrete strategy type (must satisfy StrategyPolicy).
///                           Strategy::kMaxSymbols determines symbol array sizes.
/// @param base               Pointer to buffer (>= strategy_ctx_buf_size bytes).
/// @param tcp_rx_size        TCP streaming recv buffer size.
/// @param tcp_tx_size        TCP send buffer size.
/// @param scratch_size       Serialization scratch size (must be >= max message
///                           wire size; see protocol.hpp for per-type sizes).
/// @param max_outstanding    Maximum concurrent outstanding orders (global).
/// @param max_outstanding_per_symbol  Per-symbol outstanding limit.
/// @param timeout_wheel_size TimingWheel wheel_size (power-of-2).
/// @param timeout_max_timers TimingWheel max_timers (power-of-2).
template <StrategyPolicy Strategy>
[[nodiscard]] inline StrategyCtx
make_strategy_ctx(std::byte *base,
                  std::size_t tcp_rx_size,
                  std::size_t tcp_tx_size,
                  std::size_t scratch_size,
                  std::uint32_t max_outstanding,
                  std::uint32_t max_outstanding_per_symbol,
                  std::size_t timeout_wheel_size,
                  std::size_t timeout_max_timers) noexcept {
  constexpr std::uint32_t kMaxSymbols = Strategy::kMaxSymbols;
  std::size_t offset = 0;

  // -- TCP I/O --

  auto *tcp_rx = reinterpret_cast<char *>(base + offset);
  offset = sys::align_up(offset + tcp_rx_size, sys::kCacheLineSize);

  auto *tcp_tx = base + offset;
  offset = sys::align_up(offset + tcp_tx_size, sys::kCacheLineSize);

  auto *scratch = base + offset;
  offset += scratch_size;

  // -- OMS state --

  offset = sys::align_up(offset, alignof(std::max_align_t));

  // net_position
  auto *net_pos = reinterpret_cast<std::int64_t *>(base + offset);
  offset += kMaxSymbols * sizeof(std::int64_t);
  std::memset(net_pos, 0, kMaxSymbols * sizeof(std::int64_t));

  // outstanding_per_symbol
  offset = sys::align_up(offset, alignof(std::uint32_t));
  auto *os_per_sym = reinterpret_cast<std::uint32_t *>(base + offset);
  offset += kMaxSymbols * sizeof(std::uint32_t);
  std::memset(os_per_sym, 0, kMaxSymbols * sizeof(std::uint32_t));

  // active_orders
  offset = sys::align_up(offset, alignof(ActiveOrder));
  auto *active = reinterpret_cast<ActiveOrder *>(base + offset);
  const std::size_t active_bytes =
      kMaxSymbols * StrategyCtx::kSideCount * sizeof(ActiveOrder);
  offset += active_bytes;
  std::memset(active, 0, active_bytes);

  // cancel_buf
  offset = sys::align_up(offset, alignof(CancelOrder));
  auto *cancel = reinterpret_cast<CancelOrder *>(base + offset);
  offset += max_outstanding * sizeof(CancelOrder);

  // map_slots
  const auto map_cap = order_ctx_map_capacity(max_outstanding);
  offset = sys::align_up(offset, OutstandingMap::slot_alignment());
  auto *map = base + offset;
  const auto map_size = OutstandingMap::required_buffer_size(map_cap);
  offset += map_size;

  // timeout_ids
  offset = sys::align_up(offset, alignof(std::uint64_t));
  auto *tids = reinterpret_cast<std::uint64_t *>(base + offset);
  offset += max_outstanding * sizeof(std::uint64_t);

  // timeout_wheel_buf
  offset = sys::align_up(offset, alignof(std::max_align_t));
  auto *wheel_buf = base + offset;
  const auto wheel_buf_size = ds::TimingWheel::required_buffer_size(
      timeout_wheel_size, timeout_max_timers);

  return {
      .tcp_rx = tcp_rx,
      .tcp_rx_size = tcp_rx_size,
      .tcp_tx = {tcp_tx, tcp_tx_size},
      .scratch = {scratch, scratch_size},
      .net_position = net_pos,
      .outstanding_per_symbol = os_per_sym,
      .active_orders = active,
      .map_buf = map,
      .map_buf_size = map_size,
      .map_capacity = map_cap,
      .cancel_buf = cancel,
      .timeout_wheel_buf = wheel_buf,
      .timeout_wheel_buf_size = wheel_buf_size,
      .timeout_wheel_size = timeout_wheel_size,
      .timeout_max_timers = timeout_max_timers,
      .timeout_ids = tids,
      .max_symbols = kMaxSymbols,
      .max_outstanding = max_outstanding,
      .max_outstanding_per_symbol = max_outstanding_per_symbol,
  };
}

} // namespace mk::app
