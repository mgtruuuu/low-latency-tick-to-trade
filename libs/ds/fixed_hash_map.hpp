/**
 * @file fixed_hash_map.hpp
 * @brief Fixed-capacity, open-addressing hash map with linear probing.
 *
 * Zero-allocation hash map for hot-path use. All storage is inline
 * (std::array), so the entire map lives in a single contiguous allocation — no
 * heap pointers, no indirection, no cache misses chasing linked-list chains.
 *
 * Design choices:
 *
 *   Open addressing vs. chaining:
 *     Chaining (std::unordered_map) allocates a linked-list node per entry,
 *     causing random memory accesses and cache misses. Open addressing stores
 *     everything in a flat array, so probing hits consecutive cache lines.
 *     For HFT maps with known max sizes, open addressing wins on latency.
 *
 *   Linear probing:
 *     Simplest probe sequence. With a good hash (mix64), clustering is minimal
 *     at load factors below 70%. Robin Hood or quadratic probing would reduce
 *     worst-case probe length but add branch complexity on the hot path.
 *
 *   Power-of-two capacity:
 *     Allows bitwise AND masking (index & mask) instead of expensive modulo.
 *     Requires a good hash to avoid clustering from correlated low bits —
 *     DefaultHash applies mix64 to solve this.
 *
 *   AoS layout (Slot = {key, value, state}):
 *     Keeps key and value adjacent for cache locality on successful lookups.
 *     SoA (separate key[], value[], state[] arrays) would be better when
 *     scanning many keys without reading values (e.g., existence checks),
 *     but AoS is simpler and sufficient for most HFT use cases.
 *
 *   Tombstone deletion:
 *     Erased slots are marked kTombstone rather than kEmpty. This preserves
 *     probe chains: if slot [A, B, _, C] has B erased to empty, a lookup
 *     for C would stop at the empty slot and miss C. Tombstones are skipped
 *     during probing but can be reused by insert.
 *
 * Template parameters:
 *   Key          — Key type (must be nothrow copy-assignable, nothrow
 *                  default-constructible, trivially destructible)
 *   Value        — Value type (same requirements as Key)
 *   CapacityPow2 — Table size, must be power-of-two >= 4
 *   Hash         — Hash functor, must be nothrow-invocable
 *                  (default: DefaultHash<Key> with mix64)
 *   KeyEqual     — Key comparison functor, must be nothrow-invocable
 *                  (default: std::equal_to<> — transparent comparator)
 */

#pragma once

#include "ds/hash_utils.hpp"
#include "sys/bit_utils.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>     // std::abort
#include <functional>  // std::equal_to
#include <type_traits> // std::is_nothrow_copy_assignable_v, etc.

namespace mk::ds {

// Default KeyEqual uses std::equal_to<> (the C++14 transparent comparator,
// a.k.a. "diamond functor") instead of std::equal_to<Key>. The non-void
// specialization is a legacy C++98 artifact whose operator() is NOT marked
// noexcept in libstdc++ (LWG 3828, still open as of C++23). The void
// specialization uses a conditional noexcept that propagates from the
// underlying operator==, which is noexcept for all sane key types.
template <class Key, class Value, std::size_t CapacityPow2,
          class Hash = DefaultHash<Key>, class KeyEqual = std::equal_to<>>
class FixedHashMap {
  // ---------------------------------------------------------------------------
  // Compile-time validation
  // ---------------------------------------------------------------------------
  static_assert(CapacityPow2 >= 4,
                "Capacity must be at least 4 (smaller tables have degenerate "
                "load factor behavior)");
  static_assert(
      mk::sys::is_power_of_two(static_cast<std::uint32_t>(CapacityPow2)),
      "Capacity must be a power of two (for bitwise masking)");
  static_assert(std::is_nothrow_copy_assignable_v<Key>,
                "Key must be nothrow copy-assignable (used in slot writes)");
  static_assert(std::is_nothrow_copy_assignable_v<Value>,
                "Value must be nothrow copy-assignable (used in slot writes)");

  // Slots are default-initialized in std::array, so Key/Value must be
  // nothrow default-constructible.
  static_assert(std::is_nothrow_default_constructible_v<Key>,
                "Key must be nothrow default-constructible (Slot storage is "
                "default-initialized)");
  static_assert(std::is_nothrow_default_constructible_v<Value>,
                "Value must be nothrow default-constructible (Slot storage is "
                "default-initialized)");

  // erase() and clear() flip SlotState without calling destructors.
  // This is safe and correct only for trivially-destructible types —
  // the HFT use case (integers, small POD structs) naturally satisfies this.
  static_assert(std::is_trivially_destructible_v<Key>,
                "Key must be trivially destructible (erase/clear skip "
                "destructors for hot-path performance)");
  static_assert(std::is_trivially_destructible_v<Value>,
                "Value must be trivially destructible (erase/clear skip "
                "destructors for hot-path performance)");

  // Hash and KeyEqual are called inside noexcept functions. If they throw,
  // std::terminate() is called. Enforce nothrow at compile time.
  static_assert(std::is_nothrow_invocable_r_v<std::size_t, Hash, const Key &>,
                "Hash must be nothrow-invocable const with Key& and return "
                "std::size_t");
  static_assert(
      std::is_nothrow_invocable_r_v<bool, KeyEqual, const Key &, const Key &>,
      "KeyEqual must be nothrow-invocable with const two Key& arguments");

  // ---------------------------------------------------------------------------
  // Internal types
  // ---------------------------------------------------------------------------
  struct Slot {
    Key key{};
    Value value{};
    SlotState state{SlotState::kEmpty};
  };

  // ---------------------------------------------------------------------------
  // Compile-time constants
  // ---------------------------------------------------------------------------

  // Bitwise mask for index wrapping: index & kMask ∈ [0, CapacityPow2).
  static constexpr std::size_t kMask = CapacityPow2 - 1;

  // Maximum number of occupied + tombstone slots before rejecting new inserts.
  // 70% load factor is the sweet spot for linear probing: average probe length
  // is ~1.7 at 70% vs ~2.5 at 80% and ~5.0 at 90%.
  //
  // Note: we count (size_ + tombstones_) against this limit, not just size_.
  // Tombstones degrade probe performance just like live entries because probing
  // must skip past them. A table with 30% live + 40% tombstones probes like
  // a 70% full table.
  static constexpr std::size_t kMaxLoad = CapacityPow2 * 7 / 10;

  // When tombstone count exceeds this threshold, the map should be rebuilt
  // (re-inserted into a fresh table) to reclaim tombstone slots.
  // Checked via needs_rebuild().
  static constexpr std::size_t kTombstoneThreshold = CapacityPow2 * 2 / 10;

  // ---------------------------------------------------------------------------
  // Data members
  // ---------------------------------------------------------------------------
  std::array<Slot, CapacityPow2> slots_{};
  std::size_t size_{0};       // Number of kFull slots.
  std::size_t tombstones_{0}; // Number of kTombstone slots.

  // [[no_unique_address]]: C++20 attribute that allows empty class members
  // (like stateless functors) to occupy zero bytes. Without this, Hash and
  // KeyEqual would each consume at least 1 byte due to C++ object identity
  // rules. This is the C++20 replacement for the EBO (Empty Base Optimization)
  // trick of inheriting from the functor.
  [[no_unique_address]] Hash hash_{};
  [[no_unique_address]] KeyEqual eq_{};

  // ---------------------------------------------------------------------------
  // Internal helpers
  // ---------------------------------------------------------------------------

  /// Compute the starting bucket index for a key.
  [[nodiscard]] std::size_t bucket_for(const Key &key) const noexcept {
    return hash_(key) & kMask;
  }

  /// Find the slot containing 'key', or the first kEmpty slot (end of chain).
  /// Returns the index. Caller checks slots_[idx].state to distinguish.
  [[nodiscard]] std::size_t probe_find(const Key &key) const noexcept {
    std::size_t idx = bucket_for(key);
    for (std::size_t i = 0; i < CapacityPow2; ++i) {
      const auto &slot = slots_[idx];
      if (slot.state == SlotState::kEmpty) {
        return idx; // Key not found — chain terminated.
      }
      if (slot.state == SlotState::kFull && eq_(slot.key, key)) {
        return idx; // Found the key.
      }
      // kTombstone or non-matching kFull: continue probing.
      idx = (idx + 1) & kMask;
    }
    // Full table scan without finding key or empty slot.
    // Invariant violation: load factor guarantees at least one empty slot.
    std::abort();
  }

  /// Find a slot for inserting 'key'. Returns {index, found_existing}.
  /// If found_existing is true, slots_[index] contains the key.
  /// If false, slots_[index] is the best insertion point (first tombstone
  /// encountered, or the terminal empty slot).
  ///
  /// Note: tombstone reuse adds a branch per probe step. On a hot path where
  /// inserts are rare or rebuilds are cheap, skip tombstone reuse and let
  /// the cold-path rebuild reclaim them instead.
  struct ProbeInsertResult {
    std::size_t idx;
    bool found_existing;
  };

  [[nodiscard]] ProbeInsertResult probe_insert(const Key &key) const noexcept {
    std::size_t idx = bucket_for(key);
    // Track the first tombstone we encounter — we'll reuse it for insertion
    // to keep probe chains short.
    std::size_t first_tombstone = CapacityPow2; // sentinel = "none found"

    for (std::size_t i = 0; i < CapacityPow2; ++i) {
      const auto &slot = slots_[idx];

      if (slot.state == SlotState::kEmpty) {
        // End of probe chain. Key doesn't exist.
        // Prefer inserting at an earlier tombstone if we found one.
        const std::size_t insert_idx =
            (first_tombstone != CapacityPow2) ? first_tombstone : idx;
        return {insert_idx, false};
      }

      if (slot.state == SlotState::kTombstone) {
        // Remember first tombstone for potential reuse.
        if (first_tombstone == CapacityPow2) {
          first_tombstone = idx;
        }
      } else if (eq_(slot.key, key)) {
        // Key already exists.
        return {idx, true};
      }

      idx = (idx + 1) & kMask;
    }

    // Fallback: table fully probed without finding key or empty slot.
    // Invariant violation: load factor guarantees at least one empty slot.
    std::abort();
  }

public:
  /// Result of an upsert operation. Three-way return distinguishes successful
  /// insert, successful update, and capacity failure — avoiding the ambiguous
  /// bool return where 'false' could mean either "updated" or "failed".
  enum class UpsertResult : std::uint8_t {
    kInserted,     ///< New key was inserted.
    kUpdated,      ///< Existing key's value was overwritten.
    kCapacityFull, ///< New key rejected — load factor limit reached.
  };

  FixedHashMap() = default;
  ~FixedHashMap() = default;

  // Non-copyable, non-movable: the internal std::array<Slot, N> can be very
  // large (e.g., 1024 slots * sizeof(Slot)). Accidental copies would be
  // extremely expensive. If you need to transfer ownership, use a pointer.
  FixedHashMap(const FixedHashMap &) = delete;
  FixedHashMap &operator=(const FixedHashMap &) = delete;
  FixedHashMap(FixedHashMap &&) = delete;
  FixedHashMap &operator=(FixedHashMap &&) = delete;

  // ---------------------------------------------------------------------------
  // Capacity
  // ---------------------------------------------------------------------------

  /// Number of live key-value pairs in the map.
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  /// True if the map contains no live entries.
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

  /// Maximum number of slots (compile-time constant).
  [[nodiscard]] static constexpr std::size_t capacity() noexcept {
    return CapacityPow2;
  }

  /// Number of tombstone slots. High tombstone counts degrade probe
  /// performance.
  [[nodiscard]] std::size_t tombstone_count() const noexcept {
    return tombstones_;
  }

  /// True if tombstone count exceeds the rebuild threshold (20% of capacity).
  /// When this returns true, the caller should rebuild the map (construct a
  /// new FixedHashMap and re-insert all live entries) to reclaim tombstones.
  [[nodiscard]] bool needs_rebuild() const noexcept {
    return tombstones_ > kTombstoneThreshold;
  }

  // ---------------------------------------------------------------------------
  // Lookup
  // ---------------------------------------------------------------------------

  /// Find a key in the map. Returns a pointer to the value, or nullptr if
  /// not found. The pointer is valid until the next mutating operation.
  ///
  /// Pointer-based return (not std::optional) avoids copy overhead for large
  /// Value types and is the standard pattern in HFT hash maps.
  [[nodiscard]] Value *find(const Key &key) noexcept {
    const std::size_t idx = probe_find(key);
    auto &slot = slots_[idx];
    // probe_find guarantees: if state == kFull, the key matches.
    // No redundant eq_ check needed.
    if (slot.state == SlotState::kFull) {
      return &slot.value;
    }
    return nullptr;
  }

  [[nodiscard]] const Value *find(const Key &key) const noexcept {
    const std::size_t idx = probe_find(key);
    const auto &slot = slots_[idx];
    if (slot.state == SlotState::kFull) {
      return &slot.value;
    }
    return nullptr;
  }

  // ---------------------------------------------------------------------------
  // Insertion
  // ---------------------------------------------------------------------------

  /// Insert a key-value pair. Returns true if inserted, false if the key
  /// already exists or the table is too full (load factor exceeded).
  ///
  /// Semantics: insert-only, never overwrites. Use upsert() to update.
  [[nodiscard]] bool insert(const Key &key, const Value &value) noexcept {
    auto [idx, found_existing] = probe_insert(key);

    if (found_existing) {
      return false; // Key already present — no overwrite.
    }

    // Check load factor AFTER confirming key doesn't exist.
    // This avoids rejecting updates to existing keys, which is a common
    // bug in naive implementations (the study doc mentions this caveat).
    //
    // We count tombstones toward load because they degrade probe performance.
    // However, if we're reusing a tombstone slot, the effective load doesn't
    // increase (we're replacing a tombstone, not consuming an empty slot).
    const bool reusing_tombstone = slots_[idx].state == SlotState::kTombstone;
    if (!reusing_tombstone && (size_ + tombstones_ >= kMaxLoad)) {
      return false; // Table too full for new entries.
    }

    if (reusing_tombstone) {
      --tombstones_; // Reclaiming a tombstone slot.
    }

    slots_[idx].key = key;
    slots_[idx].value = value;
    slots_[idx].state = SlotState::kFull;
    ++size_;
    return true;
  }

  /// Insert or update a key-value pair. Returns a three-way UpsertResult:
  ///   kInserted     — new key was added to the map.
  ///   kUpdated      — existing key's value was overwritten.
  ///   kCapacityFull — new key rejected (load factor limit reached).
  ///
  /// Updates always succeed (no load factor check for existing keys).
  /// New inserts respect the load factor limit.
  [[nodiscard]] UpsertResult upsert(const Key &key,
                                    const Value &value) noexcept {
    auto [idx, found_existing] = probe_insert(key);

    if (found_existing) {
      // Key exists — overwrite value. No size/tombstone change.
      slots_[idx].value = value;
      return UpsertResult::kUpdated;
    }

    // New key — check load factor.
    const bool reusing_tombstone = slots_[idx].state == SlotState::kTombstone;
    if (!reusing_tombstone && (size_ + tombstones_ >= kMaxLoad)) {
      return UpsertResult::kCapacityFull;
    }

    if (reusing_tombstone) {
      --tombstones_;
    }

    slots_[idx].key = key;
    slots_[idx].value = value;
    slots_[idx].state = SlotState::kFull;
    ++size_;
    return UpsertResult::kInserted;
  }

  // ---------------------------------------------------------------------------
  // Deletion
  // ---------------------------------------------------------------------------

  /// Erase a key from the map. Returns true if the key was found and erased,
  /// false if the key was not present.
  ///
  /// Uses tombstone deletion: the slot is marked kTombstone rather than kEmpty
  /// to preserve probe chains for subsequent lookups.
  bool erase(const Key &key) noexcept {
    const std::size_t idx = probe_find(key);
    auto &slot = slots_[idx];
    // probe_find guarantees: if state == kFull, the key matches.
    if (slot.state != SlotState::kFull) {
      return false; // Key not found.
    }

    slot.state = SlotState::kTombstone;
    --size_;
    ++tombstones_;
    return true;
  }

  // ---------------------------------------------------------------------------
  // Bulk operations
  // ---------------------------------------------------------------------------

  /// Reset all slots to empty. O(N) in capacity.
  void clear() noexcept {
    for (auto &slot : slots_) {
      slot.state = SlotState::kEmpty;
    }
    size_ = 0;
    tombstones_ = 0;
  }

  // ---------------------------------------------------------------------------
  // Iteration (cold-path only)
  // ---------------------------------------------------------------------------

  /// Visit all live entries. Callback receives (const Key&, Value&).
  /// O(N) in capacity — scans all slots. Cold-path use only (kill switch,
  /// diagnostics, rebuild). Not suitable for hot-path iteration.
  ///
  /// The callback must not mutate the map structure (no insert/erase/upsert).
  /// Mutating the Value through the reference is allowed.
  template <class Fn>
  void for_each(Fn fn) noexcept(noexcept(fn(std::declval<const Key &>(),
                                             std::declval<Value &>()))) {
    for (auto &slot : slots_) {
      if (slot.state == SlotState::kFull) {
        fn(slot.key, slot.value);
      }
    }
  }

  /// Const overload for read-only iteration.
  template <class Fn>
  void for_each(Fn fn) const
      noexcept(noexcept(fn(std::declval<const Key &>(),
                           std::declval<const Value &>()))) {
    for (const auto &slot : slots_) {
      if (slot.state == SlotState::kFull) {
        fn(slot.key, slot.value);
      }
    }
  }
};

} // namespace mk::ds
