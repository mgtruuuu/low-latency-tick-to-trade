/**
 * @file fixed_timing_wheel.hpp
 * @brief Fixed-capacity timing wheel — O(1) schedule/cancel, O(M) tick.
 *
 * Canonical timing wheel data structure (Varghese & Lauck, 1987).
 * Provides amortized O(1) timer management for HFT hot paths:
 * order ack timeouts, heartbeat liveness, session keepalive.
 *
 * Design:
 *   - Single-level wheel with power-of-2 bucket count (bitwise mask, no
 * modulo).
 *   - Pre-allocated node pool via FixedIndexFreeStack — zero allocation after
 * construction.
 *   - Manual doubly-linked list per bucket (NOT IntrusiveList). Buckets need
 * O(1) detach-all for reentrancy safety during tick(): the entire chain is
 * detached before callbacks fire, so callbacks that schedule to the same slot
 * go into the now-empty bucket and fire on the next rotation (correct
 * behavior).
 *   - Generation counter in handles prevents ABA: a stale handle cannot cancel
 *     a timer that reused the same node slot.
 *   - Callback: void(*)(void*) noexcept — zero-allocation C-style function
 * pointer with opaque context. The wheel does NOT own the context memory; the
 * caller must ensure ctx outlives the timer or cancel before destruction.
 *   - Single-threaded, single-owner. No locks, no atomics. For cross-thread
 *     access, use a command queue (SPSC ring) to feed schedule/cancel requests
 *     to the owning thread.
 *
 * Constraints:
 *   - WheelSize and MaxTimers must be powers of two.
 *   - delay must be in [1, WheelSize) — delays >= WheelSize wrap and fire
 * early. Use a larger WheelSize or a rounds counter extension for longer
 * delays.
 *   - Non-copyable, non-movable: node pointers form an internal reference web.
 *
 * Usage:
 *   mk::ds::FixedTimingWheel<256, 1024> wheel;  // 256 slots, up to 1024
 * concurrent timers
 *
 *   auto h = wheel.schedule([](void* ctx) noexcept {
 *     auto* order = static_cast<Order*>(ctx);
 *     order->on_timeout();
 *   }, &my_order, 50);  // fire in 50 ticks
 *
 *   // In event loop:
 *   wheel.tick();       // advance one tick, fires expired timers
 *
 *   // Cancel before firing:
 *   wheel.cancel(h);    // returns true if cancelled, false if stale/already
 * fired
 */

#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "ds/fixed_index_free_stack.hpp"
#include "sys/bit_utils.hpp"

namespace mk::ds {

template <std::size_t WheelSize, std::size_t MaxTimers> class FixedTimingWheel {
  static_assert(mk::sys::is_power_of_two(WheelSize),
                "WheelSize must be a power of two");
  static_assert(WheelSize > 1, "WheelSize must be > 1 (at least 2 slots)");
  static_assert(mk::sys::is_power_of_two(MaxTimers),
                "MaxTimers must be a power of two");
  static_assert(MaxTimers > 0, "MaxTimers must be > 0");

public:
  using tick_t = std::uint64_t;
  using handle_t = std::uint64_t;

  /// Callback type: function pointer + opaque context. Must be noexcept.
  /// The wheel never allocates for callbacks — this is a direct 2-word storage
  /// (function pointer + void*), unlike std::function which may heap-allocate
  /// for large captures. Zero overhead, predictable on hot path.
  using cb_t = void (*)(void *) noexcept;

  /// Invalid handle constant. schedule() returns this on pool exhaustion.
  static constexpr handle_t kInvalidHandle{0};

  // ===========================================================================
  // Construction
  // ===========================================================================

  FixedTimingWheel() noexcept = default;

  /// Non-copyable, non-movable: node pointers form an internal reference web.
  /// Moving would invalidate all prev/next pointers in the bucket chains.
  FixedTimingWheel(const FixedTimingWheel &) = delete;
  FixedTimingWheel &operator=(const FixedTimingWheel &) = delete;
  FixedTimingWheel(FixedTimingWheel &&) = delete;
  FixedTimingWheel &operator=(FixedTimingWheel &&) = delete;

  ~FixedTimingWheel() noexcept {
    // Unlink all armed nodes so the pool is clean.
    // No callbacks are fired — the caller should cancel all timers before
    // destruction, or accept that contexts may leak.
    reset();
  }

  // ===========================================================================
  // Core operations
  // ===========================================================================

  /// Schedule a timer to fire after `delay` ticks.
  ///
  /// @param cb     Callback to invoke when the timer fires.
  /// @param ctx    Opaque context passed to cb. Caller owns lifetime.
  /// @param delay  Ticks until firing. Must be in [1, WheelSize).
  /// @return Handle for cancellation, or kInvalidHandle if pool exhausted.
  ///
  /// O(1): pop from free list + push_front into bucket.
  [[nodiscard]] handle_t schedule(cb_t cb, void *ctx, tick_t delay) noexcept {
    // Precondition violations corrupt the wheel (wrong slot, null call) —
    // must abort even in release builds.
    if (delay == 0 || delay >= WheelSize || cb == nullptr) [[unlikely]] {
      std::abort();
    }

    // Allocate a node from the pool.
    auto opt_idx = free_list_.pop();
    if (!opt_idx) [[unlikely]] {
      return kInvalidHandle;
    }

    const auto idx = *opt_idx;
    TimerNode &node = nodes_[idx];

    // Initialize node payload.
    node.cb = cb;
    node.ctx = ctx;
    const auto slot =
        static_cast<std::uint32_t>((current_tick_ + delay) & kWheelMask);
    node.slot = slot;

    // Push front into bucket's doubly-linked chain.
    push_front(slot, idx);

    ++active_count_;
    return pack_handle(idx, node.gen);
  }

  /// Cancel a timer by handle.
  ///
  /// @param h  Handle returned by schedule().
  /// @return true if the timer was armed and is now cancelled.
  ///         false if the handle is invalid, stale, or the timer already fired.
  ///
  /// O(1): unlink from bucket + push to free list.
  bool cancel(handle_t h) noexcept {
    if (h == kInvalidHandle) [[unlikely]] {
      return false;
    }

    const auto [idx, gen] = unpack_handle(h);

    // Bounds check.
    if (idx >= MaxTimers) [[unlikely]] {
      return false;
    }

    TimerNode &node = nodes_[idx];

    // Generation mismatch → handle is stale (node was freed and reused).
    if (node.gen != gen) {
      return false;
    }

    // Not armed (cb == nullptr) → either free or currently being fired
    // in tick() (self-cancel via cleanup function). Either way, no-op.
    if (!is_armed(node)) {
      return false;
    }

    if (in_tick_walk_) {
      // During tick() walk: mark as cancelled WITHOUT unlinking.
      // tick() walks the detached chain via next pointers. If we called
      // unlink() here, it would set next = kNullIdx and orphan all
      // subsequent nodes in the chain. Instead, we just clear cb/ctx
      // so tick() can detect and skip this node while continuing the walk.
      //
      // Real-world trigger: order ack timeout callback cancels the
      // associated risk-check timer; session disconnect callback cancels
      // all heartbeat timers for that session.
      node.cb = nullptr;
      node.ctx = nullptr;
      --active_count_;
      return true;
    }

    // Normal path (outside tick walk): unlink from bucket + free.
    unlink(idx);
    free_node(idx);
    --active_count_;
    return true;
  }

  /// Advance the wheel by one tick. Fires all timers whose deadline is now.
  ///
  /// @return Number of callbacks fired.
  ///
  /// Semantics: delay=N means the timer fires after exactly N tick() calls.
  /// tick() increments current_tick_ first, then fires the matching bucket.
  ///
  /// O(M) where M = number of timers in the current bucket.
  /// Uses detach-then-walk pattern for reentrancy safety: the bucket is
  /// emptied before any callbacks fire, so timers scheduled by callbacks
  /// to the same slot go into the now-empty bucket (fire on next rotation).
  std::uint32_t tick() noexcept {
    ++current_tick_;
    const auto slot = static_cast<std::size_t>(current_tick_) & kWheelMask;

    // O(1) detach: take the chain head, clear the bucket.
    std::uint32_t head_idx = bucket_heads_[slot];
    bucket_heads_[slot] = kNullIdx;

    in_tick_walk_ = true;
    std::uint32_t fired = 0;

    while (head_idx != kNullIdx) {
      const auto cur_idx = head_idx;
      TimerNode &cur = nodes_[cur_idx];
      head_idx = cur.next;

      // Prefetch next node while processing current.
      // __builtin_prefetch(addr, rw, locality)
      //   rw:       0 = read, 1 = write
      //   locality: 0 = non-temporal, 1 = L2, 2 = L1+L2, 3 = all caches
      if (head_idx != kNullIdx) [[likely]] {
        __builtin_prefetch(&nodes_[head_idx], 0, 1);
      }

      // Skip nodes cancelled by an earlier callback during this walk.
      // cancel() sets cb = nullptr without unlinking, preserving the chain
      // so we can continue walking to subsequent nodes via head_idx.
      if (cur.cb == nullptr) {
        cur.prev = kNullIdx;
        cur.next = kNullIdx;
        free_node(cur_idx);
        continue; // Not counted as "fired" — callback was never invoked.
      }

      // Clear links — node is detached from the chain.
      cur.prev = kNullIdx;
      cur.next = kNullIdx;

      // Save cb/ctx locally and clear node — node is now in "running" state
      // (is_armed returns false because cb == nullptr). This prevents
      // self-cancel (e.g., cleanup function cancels all handles including
      // the currently-firing one).
      const auto cb = cur.cb;
      const auto ctx = cur.ctx;
      cur.cb = nullptr;
      cur.ctx = nullptr;

      // Save generation to detect if the callback reuses this node
      // (e.g., callback schedules a new timer that gets this same slot).
      const auto saved_gen = cur.gen;

      // Fire callback.
      cb(ctx);

      // Only free the node if the callback didn't already reuse it.
      // If gen changed, the callback freed/rescheduled this node — don't
      // double-free.
      if (cur.gen == saved_gen) {
        free_node(cur_idx);
      }

      ++fired;
    }

    in_tick_walk_ = false;
    active_count_ -= fired;
    return fired;
  }

  /// Cancel all timers and reset the tick counter to 0.
  /// No callbacks are fired. Active count drops to 0.
  void reset() noexcept {
    // Walk every bucket and free all nodes without firing callbacks.
#ifndef NDEBUG
    std::uint32_t freed = 0;
#endif
    for (std::size_t slot = 0; slot < WheelSize; ++slot) {
      std::uint32_t idx = bucket_heads_[slot];
      while (idx != kNullIdx) {
        const auto next_idx = nodes_[idx].next;
        nodes_[idx].prev = kNullIdx;
        nodes_[idx].next = kNullIdx;
        free_node(idx);
#ifndef NDEBUG
        ++freed;
#endif
        idx = next_idx;
      }
      bucket_heads_[slot] = kNullIdx;
    }
    assert(freed == active_count_ && "freed count must match active_count_");
    active_count_ = 0;
    current_tick_ = 0;
  }

  // ===========================================================================
  // Observers
  // ===========================================================================

  /// Current tick counter (monotonically increasing).
  [[nodiscard]] tick_t current_tick() const noexcept { return current_tick_; }

  /// Number of currently armed (scheduled, not yet fired/cancelled) timers.
  [[nodiscard]] std::uint32_t active_count() const noexcept {
    return active_count_;
  }

  /// Compile-time wheel size (number of buckets).
  [[nodiscard]] static constexpr std::size_t wheel_size() noexcept {
    return WheelSize;
  }

  /// Compile-time maximum concurrent timer count.
  [[nodiscard]] static constexpr std::size_t max_timers() noexcept {
    return MaxTimers;
  }

private:
  // ===========================================================================
  // Constants
  // ===========================================================================

  static constexpr std::size_t kWheelMask = WheelSize - 1;

  /// Null index sentinel. We use uint32_t max as "no node" marker.
  /// FixedIndexFreeStack returns indices in [0, MaxTimers), so this is safe.
  static constexpr std::uint32_t kNullIdx =
      std::numeric_limits<std::uint32_t>::max();

  // ===========================================================================
  // Internal node (index-based doubly-linked list)
  // ===========================================================================
  //
  // Uses 32-bit indices instead of 64-bit pointers for the bucket chain.
  // Better cache density: 4 bytes vs 8 bytes per link, and indices are
  // relative to the nodes_ array (no base-pointer arithmetic needed in
  // the hot path — just array subscript).

  struct TimerNode {
    // Hot fields (accessed every tick() iteration):
    std::uint32_t next{kNullIdx}; // next node in bucket chain (walk order)
    cb_t cb{nullptr};             // callback function pointer
    void *ctx{nullptr};           // opaque context (caller owns)
    // Warm field (accessed on cancel path):
    std::uint32_t gen{1}; // generation counter (ABA prevention)
    // Cold fields (cancel-only):
    std::uint32_t prev{kNullIdx}; // prev node in bucket chain (unlink)
    std::uint32_t slot{0};        // bucket index (for unlink during cancel)
  };

  // ===========================================================================
  // Handle encoding
  // ===========================================================================
  //
  // Handle = [gen:32][index:32].
  // gen occupies the upper 32 bits, index the lower 32.
  // kInvalidHandle == 0, so gen==0 is reserved (gen starts at 1 and
  // skips 0 on wrap to ensure handles are always non-zero when valid).

  [[nodiscard]] static handle_t pack_handle(std::uint32_t idx,
                                             std::uint32_t gen) noexcept {
    return (static_cast<handle_t>(gen) << 32) | static_cast<handle_t>(idx);
  }

  struct UnpackedHandle {
    std::uint32_t idx;
    std::uint32_t gen;
  };

  [[nodiscard]] static UnpackedHandle unpack_handle(handle_t h) noexcept {
    return {
        .idx = static_cast<std::uint32_t>(h & 0xFFFF'FFFF),
        .gen = static_cast<std::uint32_t>(h >> 32),
    };
  }

  // ===========================================================================
  // Bucket chain operations (manual doubly-linked list with index)
  // ===========================================================================

  /// Is this timer still armed (scheduled, not yet fired/cancelled)?
  /// Uses cb pointer as the armed indicator: armed nodes have cb != nullptr.
  /// tick() clears cb before firing; free_node() clears cb on recycle.
  [[nodiscard]] static bool is_armed(const TimerNode &node) noexcept {
    return node.cb != nullptr;
  }

  /// Push a node to the front of bucket[slot]'s chain.
  void push_front(std::uint32_t slot, std::uint32_t idx) noexcept {
    TimerNode &node = nodes_[idx];
    node.prev = kNullIdx; // new head has no predecessor

    const auto old_head = bucket_heads_[slot];
    node.next = old_head;

    if (old_head != kNullIdx) {
      nodes_[old_head].prev = idx;
    }

    bucket_heads_[slot] = idx;
  }

  /// Unlink a node from its bucket chain.
  void unlink(std::uint32_t idx) noexcept {
    TimerNode &node = nodes_[idx];
    const auto slot = node.slot;

    if (node.prev != kNullIdx) {
      nodes_[node.prev].next = node.next;
    } else {
      // Node is the bucket head.
      bucket_heads_[slot] = node.next;
    }

    if (node.next != kNullIdx) {
      nodes_[node.next].prev = node.prev;
    }

    node.prev = kNullIdx;
    node.next = kNullIdx;
  }

  // ===========================================================================
  // Node pool management
  // ===========================================================================

  /// Return a node to the free list. Increments generation counter.
  void free_node(std::uint32_t idx) noexcept {
    TimerNode &node = nodes_[idx];

    // Bump generation — makes any outstanding handle for this slot stale.
    ++node.gen;
    if (node.gen == 0) [[unlikely]] {
      // Skip gen==0 to ensure pack_handle never produces kInvalidHandle (0).
      // With 32-bit gen, wrap happens after ~4 billion reuses per slot —
      // practically never, but correctness demands it.
      node.gen = 1;
    }

    // Clear payload — cb==nullptr signals "not armed" (used by is_armed).
    node.cb = nullptr;
    node.ctx = nullptr;

    free_list_.push(idx);
  }

  // ===========================================================================
  // Data members
  // ===========================================================================

  /// Bucket heads: index of the first node in each bucket's chain.
  /// kNullIdx means the bucket is empty.
  std::array<std::uint32_t, WheelSize> bucket_heads_{[]() {
    std::array<std::uint32_t, WheelSize> a{};
    a.fill(kNullIdx);
    return a;
  }()};

  /// Pre-allocated node pool. Indexed by FixedIndexFreeStack.
  std::array<TimerNode, MaxTimers> nodes_{};

  /// Free list for node indices. O(1) pop/push.
  FixedIndexFreeStack<MaxTimers> free_list_{};

  /// Monotonically increasing tick counter.
  tick_t current_tick_{0};

  /// Number of currently armed timers.
  std::uint32_t active_count_{0};

  /// True while tick() is walking a detached chain.
  /// When set, cancel() marks nodes as cancelled (cb = nullptr) WITHOUT
  /// unlinking, so the chain stays intact for tick() to continue walking.
  bool in_tick_walk_{false};
};

} // namespace mk::ds
