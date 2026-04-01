/**
 * @file hash_map.hpp
 * @brief Runtime-capacity, open-addressing hash map with caller-managed
 * storage.
 *
 * Counterpart to FixedHashMap<K, V, N>: same probing algorithm and semantics,
 * but capacity is a runtime value and the slot buffer is provided by the caller
 * (stack array, heap allocation, MmapRegion, shared memory, etc.).
 *
 * Relationship to FixedHashMap:
 *   FixedHashMap : HashMap  ≈  std::array : std::span
 *   Compile-time capacity → inline std::array storage, constexpr constants.
 *   Runtime capacity → pointer-based storage, runtime constants.
 *   Same hash map protocol — the logic is independent of where memory lives.
 *
 * When to use each:
 *   FixedHashMap — Small, known-size maps (< ~4K entries). Inline storage
 *     avoids indirection and keeps the map on the same cache lines as its
 * owner. HashMap — Large maps (10K+ entries), or when the buffer must live in
 *     a specific memory region (huge pages, NUMA, shared memory).
 *
 * Memory ownership:
 *   HashMap NEVER owns memory. The caller allocates and frees the buffer.
 *   This follows the FixedIndexFreeStack principle: operate on external
 * storage, don't own what you index. Use required_buffer_size() and
 * round_up_capacity() to compute allocation parameters, then pass a void*
 * buffer to create() or the direct constructor.
 *
 * Design: identical to FixedHashMap (linear probing, 70% load factor,
 * tombstone deletion, power-of-two masking). See fixed_hash_map.hpp for
 * detailed algorithm commentary.
 */

#pragma once

#include "ds/hash_utils.hpp" // SlotState, DefaultHash, mix64
#include "sys/bit_utils.hpp" // is_power_of_two, round_up_pow2

#include <cstddef>
#include <cstdint>
#include <cstdlib>    // std::abort
#include <functional> // std::equal_to
#include <limits>     // std::numeric_limits
#include <optional>
#include <type_traits>
#include <utility> // std::swap

namespace mk::ds {

template <class Key, class Value, class Hash = DefaultHash<Key>,
          class KeyEqual = std::equal_to<>>
class HashMap {
  // ---------------------------------------------------------------------------
  // Compile-time validation (same constraints as FixedHashMap)
  // ---------------------------------------------------------------------------
  static_assert(std::is_nothrow_copy_assignable_v<Key>,
                "Key must be nothrow copy-assignable (used in slot writes)");
  static_assert(std::is_nothrow_copy_assignable_v<Value>,
                "Value must be nothrow copy-assignable (used in slot writes)");

  // Trivially default constructible ensures Key/Value are implicit-lifetime
  // types in C++20 (P0593R6). This makes external buffer storage well-defined:
  // the runtime implicitly creates objects of implicit-lifetime types, so slot
  // lifetimes begin without explicit construction.
  static_assert(std::is_trivially_default_constructible_v<Key>,
                "Key must be trivially default-constructible (external buffer "
                "storage relies on implicit object creation in C++20)");
  static_assert(
      std::is_trivially_default_constructible_v<Value>,
      "Value must be trivially default-constructible (external buffer "
      "storage relies on implicit object creation in C++20)");

  static_assert(std::is_trivially_destructible_v<Key>,
                "Key must be trivially destructible (erase/clear skip "
                "destructors for hot-path performance)");
  static_assert(std::is_trivially_destructible_v<Value>,
                "Value must be trivially destructible (erase/clear skip "
                "destructors for hot-path performance)");

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
  // Data members
  // ---------------------------------------------------------------------------
  Slot *slots_{nullptr};
  std::size_t capacity_{0};
  std::size_t mask_{0};                // capacity_ - 1
  std::size_t max_load_{0};            // capacity_ * 7 / 10
  std::size_t tombstone_threshold_{0}; // capacity_ * 2 / 10
  std::size_t size_{0};
  std::size_t tombstones_{0};

  [[no_unique_address]] Hash hash_{};
  [[no_unique_address]] KeyEqual eq_{};

  // ---------------------------------------------------------------------------
  // Internal helpers
  // ---------------------------------------------------------------------------

  [[nodiscard]] std::size_t bucket_for(const Key &key) const noexcept {
    return hash_(key) & mask_;
  }

  /// Find the slot containing 'key', or the first kEmpty slot (end of chain).
  [[nodiscard]] std::size_t probe_find(const Key &key) const noexcept {
    std::size_t idx = bucket_for(key);
    for (std::size_t i = 0; i < capacity_; ++i) {
      const auto &slot = slots_[idx];
      if (slot.state == SlotState::kEmpty) {
        return idx;
      }
      if (slot.state == SlotState::kFull && eq_(slot.key, key)) {
        return idx;
      }
      idx = (idx + 1) & mask_;
    }
    // Invariant violation: load factor guarantees at least one empty slot.
    std::abort();
  }

  /// Find a slot for inserting 'key'. Returns {index, found_existing}.
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
    std::size_t first_tombstone = capacity_; // sentinel = "none found"

    for (std::size_t i = 0; i < capacity_; ++i) {
      const auto &slot = slots_[idx];

      if (slot.state == SlotState::kEmpty) {
        const std::size_t insert_idx =
            (first_tombstone != capacity_) ? first_tombstone : idx;
        return {insert_idx, false};
      }

      if (slot.state == SlotState::kTombstone) {
        if (first_tombstone == capacity_) {
          first_tombstone = idx;
        }
      } else if (eq_(slot.key, key)) {
        return {idx, true};
      }

      idx = (idx + 1) & mask_;
    }

    // Invariant violation: load factor guarantees at least one empty slot.
    std::abort();
  }

  // Private constructor — used by create() factory.
  HashMap(Slot *raw_ptr, std::size_t cap) noexcept
      : slots_(raw_ptr), capacity_(cap), mask_(cap - 1),
        max_load_(cap * 7 / 10), tombstone_threshold_(cap * 2 / 10) {}

  [[nodiscard]] static constexpr bool
  is_valid_capacity(std::size_t c) noexcept {
    return c >= 4 && mk::sys::is_power_of_two(static_cast<std::uint32_t>(c)) &&
           c <= std::numeric_limits<std::uint32_t>::max();
  }


public:
  /// Result of an upsert operation. Three-way return distinguishes successful
  /// insert, successful update, and capacity failure — avoiding the ambiguous
  /// bool return where 'false' could mean either "updated" or "failed".
  /// Matches FixedHashMap::UpsertResult for API consistency.
  enum class UpsertResult : std::uint8_t {
    kInserted,     ///< New key was inserted.
    kUpdated,      ///< Existing key's value was overwritten.
    kCapacityFull, ///< New key rejected — load factor limit reached.
  };

  // ===========================================================================
  // Static helpers
  // ===========================================================================

  /// Round up to the smallest power-of-two capacity >= n, with minimum 4.
  /// Returns 0 for n == 0 (error).
  [[nodiscard]] static constexpr std::size_t
  round_up_capacity(std::size_t n) noexcept {
    if (n == 0) {
      return 0;
    }
    if (n <= 4) {
      return 4;
    }
    // Capacity is limited to uint32_t range (consistent with
    // is_valid_capacity). Without this guard, the cast silently truncates and
    // creates a smaller map.
    if (n > std::numeric_limits<std::uint32_t>::max()) {
      return 0;
    }
    return mk::sys::round_up_pow2(static_cast<std::uint32_t>(n));
  }

  /// Size of one internal slot in bytes. Needed to allocate external buffers.
  [[nodiscard]] static constexpr std::size_t slot_size() noexcept {
    return sizeof(Slot);
  }

  /// Alignment requirement for Slot. Needed when carving HashMap storage
  /// from a larger contiguous region (e.g., OrderCtx buffer layout).
  [[nodiscard]] static constexpr std::size_t slot_alignment() noexcept {
    return alignof(Slot);
  }

  /// Compute the required buffer size in bytes for a given capacity.
  [[nodiscard]] static constexpr std::size_t
  required_buffer_size(std::size_t capacity) noexcept {
    return capacity * sizeof(Slot);
  }

  // ===========================================================================
  // Safe Factory (returns std::optional — never aborts)
  // ===========================================================================

  /// Factory — caller supplies raw memory buffer.
  /// The caller is responsible for the buffer's lifetime and deallocation.
  ///
  /// @param external_buf  Pointer to buffer (at least required_buffer_size()
  ///                      bytes, aligned to alignof(Slot)).
  /// @param buf_size_bytes Size of the external buffer in bytes.
  /// @param capacity       Must be a power of two >= 4. NOT auto-rounded.
  [[nodiscard]] static std::optional<HashMap>
  create(void *external_buf, std::size_t buf_size_bytes,
         std::size_t capacity) noexcept {
    if (!is_valid_capacity(capacity) || external_buf == nullptr) {
      return std::nullopt;
    }
    // Misaligned access is UB — reject buffers not aligned to Slot boundary.
    if (reinterpret_cast<std::uintptr_t>(external_buf) % alignof(Slot) != 0) {
      return std::nullopt;
    }
    const std::size_t size_in_bytes = capacity * sizeof(Slot);
    if (buf_size_bytes < size_in_bytes) {
      return std::nullopt;
    }

    auto map = HashMap(static_cast<Slot *>(external_buf), capacity);
    map.clear(); // External buffer content is unknown — must initialize.
    return map;
  }

  // ===========================================================================
  // Direct Constructor (aborts on invalid input — startup-time use)
  // ===========================================================================

  /// Default constructor — creates an empty, unusable map (capacity == 0).
  /// Exists to support move assignment patterns.
  HashMap() noexcept = default;

  /// Direct constructor — caller supplies the buffer, aborts on invalid input.
  /// Use this at startup time when failure is unrecoverable (abort is
  /// acceptable). Prefer create() when graceful error handling is needed.
  HashMap(void *external_buf, std::size_t buf_size_bytes,
          std::size_t capacity) noexcept {
    if (!is_valid_capacity(capacity) || external_buf == nullptr ||
        buf_size_bytes < capacity * sizeof(Slot) ||
        reinterpret_cast<std::uintptr_t>(external_buf) % alignof(Slot) != 0) {
      std::abort();
    }

    capacity_ = capacity;
    mask_ = capacity - 1;
    max_load_ = capacity * 7 / 10;
    tombstone_threshold_ = capacity * 2 / 10;
    slots_ = static_cast<Slot *>(external_buf);
    clear(); // External buffer content is unknown.
  }

  // ===========================================================================
  // Move Support (safe before any concurrent use — single-threaded map)
  // ===========================================================================

  HashMap(HashMap &&other) noexcept { swap(other); }

  HashMap &operator=(HashMap &&other) noexcept {
    HashMap tmp(std::move(other));
    swap(tmp);
    return *this;
  }

  // Non-copyable: shallow copy would create aliasing bugs.
  HashMap(const HashMap &) = delete;
  HashMap &operator=(const HashMap &) = delete;

  ~HashMap() = default;

  void swap(HashMap &other) noexcept {
    std::swap(slots_, other.slots_);
    std::swap(capacity_, other.capacity_);
    std::swap(mask_, other.mask_);
    std::swap(max_load_, other.max_load_);
    std::swap(tombstone_threshold_, other.tombstone_threshold_);
    std::swap(size_, other.size_);
    std::swap(tombstones_, other.tombstones_);
    std::swap(hash_, other.hash_);
    std::swap(eq_, other.eq_);
  }

  friend void swap(HashMap &a, HashMap &b) noexcept { a.swap(b); }

  // ===========================================================================
  // Capacity
  // ===========================================================================

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t tombstone_count() const noexcept {
    return tombstones_;
  }

  [[nodiscard]] bool needs_rebuild() const noexcept {
    return tombstones_ > tombstone_threshold_;
  }

  // ===========================================================================
  // Lookup
  // ===========================================================================

  [[nodiscard]] Value *find(const Key &key) noexcept {
    if (capacity_ == 0) [[unlikely]] {
      return nullptr;
    }
    const std::size_t idx = probe_find(key);
    auto &slot = slots_[idx];
    // probe_find guarantees: if state == kFull, the key matches.
    if (slot.state == SlotState::kFull) {
      return &slot.value;
    }
    return nullptr;
  }

  [[nodiscard]] const Value *find(const Key &key) const noexcept {
    if (capacity_ == 0) [[unlikely]] {
      return nullptr;
    }
    const std::size_t idx = probe_find(key);
    const auto &slot = slots_[idx];
    if (slot.state == SlotState::kFull) {
      return &slot.value;
    }
    return nullptr;
  }

  // ===========================================================================
  // Insertion
  // ===========================================================================

  [[nodiscard]] bool insert(const Key &key, const Value &value) noexcept {
    if (capacity_ == 0) [[unlikely]] {
      return false;
    }

    auto [idx, found_existing] = probe_insert(key);

    if (found_existing) {
      return false;
    }

    const bool reusing_tombstone = slots_[idx].state == SlotState::kTombstone;
    if (!reusing_tombstone && (size_ + tombstones_ >= max_load_)) {
      return false;
    }

    if (reusing_tombstone) {
      --tombstones_;
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
    if (capacity_ == 0) [[unlikely]] {
      return UpsertResult::kCapacityFull;
    }

    auto [idx, found_existing] = probe_insert(key);

    if (found_existing) {
      slots_[idx].value = value;
      return UpsertResult::kUpdated;
    }

    const bool reusing_tombstone = slots_[idx].state == SlotState::kTombstone;
    if (!reusing_tombstone && (size_ + tombstones_ >= max_load_)) {
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

  // ===========================================================================
  // Deletion
  // ===========================================================================

  bool erase(const Key &key) noexcept {
    if (capacity_ == 0) [[unlikely]] {
      return false;
    }

    const std::size_t idx = probe_find(key);
    auto &slot = slots_[idx];
    if (slot.state != SlotState::kFull) {
      return false;
    }

    slot.state = SlotState::kTombstone;
    --size_;
    ++tombstones_;
    return true;
  }

  // ===========================================================================
  // Bulk operations
  // ===========================================================================

  /// Reset all slots to empty. O(N) in capacity.
  void clear() noexcept {
    for (std::size_t i = 0; i < capacity_; ++i) {
      slots_[i].state = SlotState::kEmpty;
    }
    size_ = 0;
    tombstones_ = 0;
  }

  // ===========================================================================
  // Iteration (cold-path only)
  // ===========================================================================

  /// Visit all live entries. Callback receives (const Key&, Value&).
  /// O(N) in capacity — scans all slots. Cold-path use only (kill switch,
  /// diagnostics, rebuild). Not suitable for hot-path iteration.
  ///
  /// The callback must not mutate the map structure (no insert/erase/upsert).
  /// Mutating the Value through the reference is allowed.
  template <class Fn>
  void for_each(Fn fn) noexcept(noexcept(fn(std::declval<const Key &>(),
                                             std::declval<Value &>()))) {
    for (std::size_t i = 0; i < capacity_; ++i) {
      if (slots_[i].state == SlotState::kFull) {
        fn(slots_[i].key, slots_[i].value);
      }
    }
  }

  /// Const overload for read-only iteration.
  template <class Fn>
  void for_each(Fn fn) const
      noexcept(noexcept(fn(std::declval<const Key &>(),
                           std::declval<const Value &>()))) {
    for (std::size_t i = 0; i < capacity_; ++i) {
      if (slots_[i].state == SlotState::kFull) {
        fn(slots_[i].key, slots_[i].value);
      }
    }
  }
};

} // namespace mk::ds
