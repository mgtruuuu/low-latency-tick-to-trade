/**
 * @file timing_wheel.hpp
 * @brief Runtime-capacity timing wheel with caller-managed storage.
 *
 * Counterpart to FixedTimingWheel<WheelSize, MaxTimers>: same algorithm and
 * semantics, but both capacities are runtime values and the backing buffer is
 * provided by the caller (stack array, heap allocation, MmapRegion, shared
 * memory, etc.).
 *
 * Relationship to FixedTimingWheel:
 *   FixedTimingWheel : TimingWheel  ≈  std::array : std::span
 *   Compile-time capacity → inline std::array storage, constexpr constants.
 *   Runtime capacity → pointer-based storage, runtime constants.
 *   Same timing wheel protocol — the logic is independent of where memory
 *   lives.
 *
 * When to use each:
 *   FixedTimingWheel — Small, known-size configurations (< ~4K timers).
 *     Inline storage avoids indirection and keeps the wheel on the same cache
 *     lines as its owner.
 *   TimingWheel — Large timer pools (10K+ timers), or when the buffer must
 *     live in a specific memory region (huge pages, NUMA, shared memory), or
 *     when capacity comes from configuration.
 *
 * Memory ownership:
 *   TimingWheel NEVER owns memory. The caller allocates and frees the buffer.
 *   Use required_buffer_size() and round_up_capacity() to compute allocation
 *   parameters, then pass a void* buffer to create() or the direct
 *   constructor.
 *
 * Design: identical to FixedTimingWheel (single-level wheel, power-of-2
 * masking, manual doubly-linked list per bucket, generation-counted handles,
 * detach-then-walk tick). See fixed_timing_wheel.hpp for detailed algorithm
 * commentary.
 */

#pragma once

#include "ds/index_free_stack.hpp"
#include "sys/bit_utils.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace mk::ds {

class TimingWheel {
public:
  using tick_t = std::uint64_t;
  using handle_t = std::uint64_t;
  using cb_t = void (*)(void *) noexcept;

  static constexpr handle_t kInvalidHandle{0};

private:
  // ===========================================================================
  // Constants
  // ===========================================================================

  static constexpr std::uint32_t kNullIdx =
      std::numeric_limits<std::uint32_t>::max();

  // ===========================================================================
  // Internal node (same layout and field order as FixedTimingWheel::TimerNode)
  // ===========================================================================

  struct TimerNode {
    // Hot fields (accessed every tick() iteration):
    std::uint32_t next{kNullIdx};
    cb_t cb{nullptr};
    void *ctx{nullptr};
    // Warm field (accessed on cancel path):
    std::uint32_t gen{1};
    // Cold fields (cancel-only):
    std::uint32_t prev{kNullIdx};
    std::uint32_t slot{0};
  };

  // ===========================================================================
  // Data members
  // ===========================================================================

  // Pointers into external buffer.
  std::uint32_t *bucket_heads_{nullptr};
  TimerNode *nodes_{nullptr};

  // Free list for timer node slot management.
  // Uses IndexFreeStack (runtime-capacity variant) backed by a region of the
  // caller-provided buffer. FixedTimingWheel uses
  // FixedIndexFreeStack<MaxTimers> (compile-time variant) for the same purpose.
  IndexFreeStack free_list_;

  // Runtime parameters.
  std::uint32_t wheel_size_{0};
  std::uint32_t wheel_mask_{0};
  std::uint32_t max_timers_{0};

  // State.
  tick_t current_tick_{0};
  std::uint32_t active_count_{0};
  bool in_tick_walk_{false};

  // ===========================================================================
  // Validation
  // ===========================================================================

  [[nodiscard]] static bool is_valid_wheel_size(std::size_t ws) noexcept {
    return ws >= 2 && ws <= std::numeric_limits<std::uint32_t>::max() &&
           mk::sys::is_power_of_two(ws);
  }

  [[nodiscard]] static bool is_valid_max_timers(std::size_t mt) noexcept {
    return mt >= 1 && mt <= std::numeric_limits<std::uint32_t>::max() &&
           mk::sys::is_power_of_two(mt);
  }

  // ===========================================================================
  // Buffer partitioning
  // ===========================================================================

  /// Compute the byte offset where nodes start (after bucket_heads + padding).
  [[nodiscard]] static constexpr std::size_t
  nodes_offset(std::size_t ws) noexcept {
    return mk::sys::align_up(ws * sizeof(std::uint32_t), alignof(TimerNode));
  }

  /// Compute the byte offset where the IndexFreeStack buffer starts.
  [[nodiscard]] static constexpr std::size_t
  free_list_offset(std::size_t ws, std::size_t mt) noexcept {
    return nodes_offset(ws) + (mt * sizeof(TimerNode));
  }

  /// Partition an external buffer into bucket_heads_, nodes_, and
  /// IndexFreeStack buffer. Returns false if the buffer is too small or
  /// misaligned.
  [[nodiscard]] static bool
  partition_buffer(void *buf, std::size_t buf_bytes, std::size_t ws,
                   std::size_t mt, std::uint32_t *&out_buckets,
                   TimerNode *&out_nodes, void *&out_free_buf,
                   std::size_t &out_free_buf_bytes) noexcept {
    if (buf == nullptr) {
      return false;
    }

    // Alignment check: TimerNode has alignof(void*) = 8 on x86-64.
    // bucket_heads (uint32_t[]) has weaker alignment, so we only need
    // to check the overall buffer alignment against TimerNode.
    if (reinterpret_cast<std::uintptr_t>(buf) % alignof(TimerNode) != 0) {
      return false;
    }

    const auto needed = required_buffer_size(ws, mt);
    if (buf_bytes < needed) {
      return false;
    }

    // Layout: [bucket_heads][padding][nodes][free_list_buffer]
    auto *raw = static_cast<std::byte *>(buf);

    out_buckets = reinterpret_cast<std::uint32_t *>(raw);
    out_nodes = reinterpret_cast<TimerNode *>(raw + nodes_offset(ws));
    out_free_buf = raw + free_list_offset(ws, mt);
    out_free_buf_bytes = IndexFreeStack::required_buffer_size(mt);

    return true;
  }

  // ===========================================================================
  // Initialization (called after buffer partitioning)
  // ===========================================================================

  void init_nodes() noexcept {
    // Fill bucket heads with kNullIdx (empty).
    for (std::uint32_t i = 0; i < wheel_size_; ++i) {
      bucket_heads_[i] = kNullIdx;
    }

    // Initialize nodes to default state.
    for (std::uint32_t i = 0; i < max_timers_; ++i) {
      nodes_[i].prev = kNullIdx;
      nodes_[i].next = kNullIdx;
      nodes_[i].cb = nullptr;
      nodes_[i].ctx = nullptr;
      nodes_[i].gen = 1;
      nodes_[i].slot = 0;
    }

    // free_list_ is self-initializing on construction — no manual init needed.
  }

  // ===========================================================================
  // Handle encoding (same as FixedTimingWheel)
  // ===========================================================================

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
  // Bucket chain operations (same as FixedTimingWheel)
  // ===========================================================================

  /// Is this timer still armed? Uses cb pointer as the armed indicator.
  [[nodiscard]] static bool is_armed(const TimerNode &node) noexcept {
    return node.cb != nullptr;
  }

  void push_front(std::uint32_t slot, std::uint32_t idx) noexcept {
    TimerNode &node = nodes_[idx];
    node.prev = kNullIdx;

    const auto old_head = bucket_heads_[slot];
    node.next = old_head;

    if (old_head != kNullIdx) {
      nodes_[old_head].prev = idx;
    }

    bucket_heads_[slot] = idx;
  }

  void unlink(std::uint32_t idx) noexcept {
    TimerNode &node = nodes_[idx];
    const auto slot = node.slot;

    if (node.prev != kNullIdx) {
      nodes_[node.prev].next = node.next;
    } else {
      bucket_heads_[slot] = node.next;
    }

    if (node.next != kNullIdx) {
      nodes_[node.next].prev = node.prev;
    }

    node.prev = kNullIdx;
    node.next = kNullIdx;
  }

  // ===========================================================================
  // Node pool management (same as FixedTimingWheel)
  // ===========================================================================

  void free_node(std::uint32_t idx) noexcept {
    TimerNode &node = nodes_[idx];

    ++node.gen;
    if (node.gen == 0) [[unlikely]] {
      node.gen = 1;
    }

    node.cb = nullptr;
    node.ctx = nullptr;

    free_list_.push(idx);
  }

public:
  // ===========================================================================
  // Static helpers
  // ===========================================================================

  /// Round up to the smallest power-of-two >= n, with minimum 2.
  /// Returns 0 for n == 0 (error).
  [[nodiscard]] static std::size_t round_up_capacity(std::size_t n) noexcept {
    if (n == 0) {
      return 0;
    }
    if (n <= 2) {
      return 2;
    }
    if (n > std::numeric_limits<std::uint32_t>::max()) {
      return 0;
    }
    return mk::sys::round_up_pow2(static_cast<std::uint32_t>(n));
  }

  /// Compute the required buffer size in bytes for given parameters.
  /// Accounts for alignment padding between sections and IndexFreeStack's
  /// buffer requirements (which include debug-only in_use_ tracker).
  [[nodiscard]] static constexpr std::size_t
  required_buffer_size(std::size_t ws, std::size_t mt) noexcept {
    // Layout: [bucket_heads: uint32_t × ws]
    //         [padding to alignof(TimerNode)]
    //         [nodes: TimerNode × mt]
    //         [IndexFreeStack buffer: see IndexFreeStack::required_buffer_size]
    return free_list_offset(ws, mt) + IndexFreeStack::required_buffer_size(mt);
  }

  // ===========================================================================
  // Safe Factory (returns std::optional — never aborts)
  // ===========================================================================

  [[nodiscard]] static std::optional<TimingWheel>
  create(void *external_buf, std::size_t buf_size_bytes, std::size_t ws,
         std::size_t mt) noexcept {
    if (!is_valid_wheel_size(ws) || !is_valid_max_timers(mt)) {
      return std::nullopt;
    }

    std::uint32_t *buckets = nullptr;
    TimerNode *nodes = nullptr;
    void *free_buf = nullptr;
    std::size_t free_buf_bytes = 0;

    if (!partition_buffer(external_buf, buf_size_bytes, ws, mt, buckets, nodes,
                          free_buf, free_buf_bytes)) {
      return std::nullopt;
    }

    auto free_list = IndexFreeStack::create(free_buf, free_buf_bytes, mt);
    if (!free_list) {
      return std::nullopt;
    }

    TimingWheel wheel;
    wheel.bucket_heads_ = buckets;
    wheel.nodes_ = nodes;
    wheel.free_list_ = std::move(*free_list);
    wheel.wheel_size_ = static_cast<std::uint32_t>(ws);
    wheel.wheel_mask_ = static_cast<std::uint32_t>(ws) - 1;
    wheel.max_timers_ = static_cast<std::uint32_t>(mt);
    wheel.init_nodes();
    return wheel;
  }

  // ===========================================================================
  // Direct Constructor (aborts on invalid input — startup-time use)
  // ===========================================================================

  /// Default constructor — empty, unusable (capacity == 0).
  /// Exists to support move assignment patterns.
  TimingWheel() noexcept = default;

  /// Direct constructor — caller supplies buffer, aborts on invalid input.
  TimingWheel(void *external_buf, std::size_t buf_size_bytes, std::size_t ws,
              std::size_t mt) noexcept {
    if (!is_valid_wheel_size(ws) || !is_valid_max_timers(mt)) {
      std::abort();
    }

    std::uint32_t *buckets = nullptr;
    TimerNode *nodes = nullptr;
    void *free_buf = nullptr;
    std::size_t free_buf_bytes = 0;

    if (!partition_buffer(external_buf, buf_size_bytes, ws, mt, buckets, nodes,
                          free_buf, free_buf_bytes)) {
      std::abort();
    }

    bucket_heads_ = buckets;
    nodes_ = nodes;
    // Direct-construct IndexFreeStack — aborts on invalid input (same policy).
    free_list_ = IndexFreeStack(free_buf, free_buf_bytes, mt);
    wheel_size_ = static_cast<std::uint32_t>(ws);
    wheel_mask_ = static_cast<std::uint32_t>(ws) - 1;
    max_timers_ = static_cast<std::uint32_t>(mt);
    init_nodes();
  }

  // ===========================================================================
  // Move Support
  // ===========================================================================

  TimingWheel(TimingWheel &&other) noexcept { swap(other); }

  TimingWheel &operator=(TimingWheel &&other) noexcept {
    TimingWheel tmp(std::move(other));
    swap(tmp);
    return *this;
  }

  // Non-copyable: shallow copy would create aliasing bugs.
  TimingWheel(const TimingWheel &) = delete;
  TimingWheel &operator=(const TimingWheel &) = delete;

  ~TimingWheel() = default;

  void swap(TimingWheel &other) noexcept {
    std::swap(bucket_heads_, other.bucket_heads_);
    std::swap(nodes_, other.nodes_);
    free_list_.swap(other.free_list_);
    std::swap(wheel_size_, other.wheel_size_);
    std::swap(wheel_mask_, other.wheel_mask_);
    std::swap(max_timers_, other.max_timers_);
    std::swap(current_tick_, other.current_tick_);
    std::swap(active_count_, other.active_count_);
    std::swap(in_tick_walk_, other.in_tick_walk_);
  }

  friend void swap(TimingWheel &a, TimingWheel &b) noexcept { a.swap(b); }

  // ===========================================================================
  // Core operations
  // ===========================================================================

  [[nodiscard]] handle_t schedule(cb_t cb, void *ctx, tick_t delay) noexcept {
    // Precondition violations corrupt the wheel — abort even in release.
    if (delay == 0 || delay >= wheel_size_ || cb == nullptr) [[unlikely]] {
      std::abort();
    }

    std::uint32_t idx = 0;
    if (!free_list_.pop(idx)) [[unlikely]] {
      return kInvalidHandle;
    }
    TimerNode &node = nodes_[idx];

    node.cb = cb;
    node.ctx = ctx;
    const auto slot =
        static_cast<std::uint32_t>((current_tick_ + delay) & wheel_mask_);
    node.slot = slot;

    push_front(slot, idx);

    ++active_count_;
    return pack_handle(idx, node.gen);
  }

  bool cancel(handle_t h) noexcept {
    if (h == kInvalidHandle) [[unlikely]] {
      return false;
    }

    const auto [idx, gen] = unpack_handle(h);

    if (idx >= max_timers_) [[unlikely]] {
      return false;
    }

    TimerNode &node = nodes_[idx];

    if (node.gen != gen) {
      return false;
    }

    if (!is_armed(node)) {
      return false;
    }

    if (in_tick_walk_) {
      // During tick() walk: mark as cancelled WITHOUT unlinking.
      // See fixed_timing_wheel.hpp cancel() for detailed commentary.
      node.cb = nullptr;
      node.ctx = nullptr;
      --active_count_;
      return true;
    }

    unlink(idx);
    free_node(idx);
    --active_count_;
    return true;
  }

  std::uint32_t tick() noexcept {
    ++current_tick_;
    const auto slot = static_cast<std::size_t>(current_tick_) & wheel_mask_;

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
      if (cur.cb == nullptr) {
        cur.prev = kNullIdx;
        cur.next = kNullIdx;
        free_node(cur_idx);
        continue;
      }

      cur.prev = kNullIdx;
      cur.next = kNullIdx;

      const auto cb = cur.cb;
      auto *const ctx = cur.ctx;
      cur.cb = nullptr;
      cur.ctx = nullptr;

      const auto saved_gen = cur.gen;

      cb(ctx);

      if (cur.gen == saved_gen) {
        free_node(cur_idx);
      }

      ++fired;
    }

    in_tick_walk_ = false;
    active_count_ -= fired;
    return fired;
  }

  void reset() noexcept {
#ifndef NDEBUG
    std::uint32_t freed = 0;
#endif
    for (std::uint32_t slot = 0; slot < wheel_size_; ++slot) {
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

  [[nodiscard]] tick_t current_tick() const noexcept { return current_tick_; }

  [[nodiscard]] std::uint32_t active_count() const noexcept {
    return active_count_;
  }

  [[nodiscard]] std::size_t wheel_size() const noexcept { return wheel_size_; }

  [[nodiscard]] std::size_t max_timers() const noexcept { return max_timers_; }
};

} // namespace mk::ds
