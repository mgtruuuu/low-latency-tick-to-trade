/**
 * @file absl_flat_hash_map_test.cpp
 * @brief GTest-based tests for absl::flat_hash_map — Swiss Table hash map.
 *
 * These tests serve as executable documentation for the absl::flat_hash_map
 * API. They cover basic CRUD operations, custom hashing via the AbslHashValue
 * protocol, and a comparison with our own FixedHashMap.
 *
 * Test plan:
 *   1. InsertAndFind        — insert, find, operator[]
 *   2. TryEmplace           — try_emplace for duplicate key handling
 *   3. EraseByKey           — erase(key) return value
 *   4. IterateAll           — range-for traversal
 *   5. ReserveAndLoadFactor — reserve(), size(), capacity()
 *   6. CustomHashWithAbslHash — AbslHashValue protocol for custom keys
 *   7. CompareWithFixedHashMap — same dataset in both maps, results match
 *   8. LargeScaleInsertAndLookup — 100k insert + full lookup
 */

#include "absl/container/flat_hash_map.h"
#include "absl/hash/hash.h"

#include "ds/fixed_hash_map.hpp"
#include "ds/hash_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// =============================================================================
// 1. InsertAndFind — basic insert, find, operator[]
// =============================================================================

TEST(AbslFlatHashMapTest, InsertAndFind) {
  absl::flat_hash_map<std::uint64_t, std::uint64_t> map;

  // insert() returns {iterator, bool}. The bool is true if insertion happened.
  auto [it1, inserted1] = map.insert({10, 100});
  EXPECT_TRUE(inserted1);
  EXPECT_EQ(it1->first, 10U);
  EXPECT_EQ(it1->second, 100U);

  // Duplicate insert: bool is false, value unchanged.
  auto [it2, inserted2] = map.insert({10, 200});
  EXPECT_FALSE(inserted2);
  EXPECT_EQ(it2->second, 100U); // Original value retained.

  // find() returns iterator; end() if not found.
  auto it = map.find(10);
  ASSERT_NE(it, map.end());
  EXPECT_EQ(it->second, 100U);

  EXPECT_EQ(map.find(999), map.end());

  // operator[] inserts a default-constructed value if key is missing.
  map[20] = 200;
  EXPECT_EQ(map[20], 200U);
  EXPECT_EQ(map.size(), 2U);

  // operator[] on non-existent key creates entry with value 0.
  auto val = map[30]; // Creates key 30 → 0.
  EXPECT_EQ(val, 0U);
  EXPECT_EQ(map.size(), 3U);
}

// =============================================================================
// 2. TryEmplace — try_emplace for duplicate key handling
// =============================================================================

TEST(AbslFlatHashMapTest, TryEmplace) {
  absl::flat_hash_map<std::string, std::string> map;

  // try_emplace constructs the value in-place, but only if the key is new.
  // Unlike insert, it won't construct the value at all for duplicate keys,
  // which avoids wasted move/copy of the value argument.
  auto [it1, ok1] = map.try_emplace("key1", "value1");
  EXPECT_TRUE(ok1);
  EXPECT_EQ(it1->second, "value1");

  // Duplicate key: value argument is NOT evaluated/moved.
  const std::string expensive_value = "value1_updated";
  auto [it2, ok2] = map.try_emplace("key1", expensive_value);
  EXPECT_FALSE(ok2);
  EXPECT_EQ(it2->second, "value1"); // Original value unchanged.

  // expensive_value was NOT moved from (try_emplace didn't consume it).
  // Note: after a failed try_emplace with std::move, the standard says the
  // value is unspecified for std::string, but absl guarantees no move happens.
  // We don't test the moved-from state since it's implementation-dependent.
}

// =============================================================================
// 3. EraseByKey — erase(key) return value
// =============================================================================

TEST(AbslFlatHashMapTest, EraseByKey) {
  absl::flat_hash_map<std::uint64_t, std::uint64_t> map = {
      {1, 10}, {2, 20}, {3, 30}};

  // erase(key) returns the number of elements removed (0 or 1 for map).
  EXPECT_EQ(map.erase(2), 1U);
  EXPECT_EQ(map.size(), 2U);
  EXPECT_EQ(map.find(2), map.end());

  // Erase non-existent key returns 0.
  EXPECT_EQ(map.erase(999), 0U);
  EXPECT_EQ(map.size(), 2U);

  // Remaining keys still present.
  EXPECT_NE(map.find(1), map.end());
  EXPECT_NE(map.find(3), map.end());
}

// =============================================================================
// 4. IterateAll — range-for traversal
// =============================================================================

TEST(AbslFlatHashMapTest, IterateAll) {
  absl::flat_hash_map<std::uint64_t, std::uint64_t> const map = {
      {1, 10}, {2, 20}, {3, 30}, {4, 40}};

  // Range-for with structured bindings (C++17).
  // Note: iteration order is NOT guaranteed — Swiss Table doesn't preserve
  // insertion order. We collect into a sorted vector for deterministic checks.
  std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
  for (const auto &[key, value] : map) {
    entries.emplace_back(key, value);
  }

  std::ranges::sort(entries);

  ASSERT_EQ(entries.size(), 4U);
  EXPECT_EQ(entries[0].first, 1U);
  EXPECT_EQ(entries[0].second, 10U);
  EXPECT_EQ(entries[1].first, 2U);
  EXPECT_EQ(entries[1].second, 20U);
  EXPECT_EQ(entries[2].first, 3U);
  EXPECT_EQ(entries[2].second, 30U);
  EXPECT_EQ(entries[3].first, 4U);
  EXPECT_EQ(entries[3].second, 40U);
}

// =============================================================================
// 5. ReserveAndLoadFactor — reserve(), size(), capacity(), load_factor()
// =============================================================================

TEST(AbslFlatHashMapTest, ReserveAndLoadFactor) {
  absl::flat_hash_map<std::uint64_t, std::uint64_t> map;

  // reserve(n) pre-allocates space for at least n elements without rehashing.
  // This is important for latency-sensitive code: avoids mid-operation rehash.
  map.reserve(1000);

  // capacity() returns the number of slots in the hash table.
  // It will be >= 1000 (typically rounded up to the next power of two or
  // group boundary for SIMD probing).
  EXPECT_GE(map.capacity(), 1000U);

  // Insert 500 elements.
  for (std::uint64_t i = 0; i < 500; ++i) {
    map[i] = i * 10;
  }
  EXPECT_EQ(map.size(), 500U);

  // load_factor() = size / capacity. Should be < 1.0.
  EXPECT_GT(map.load_factor(), 0.0F);
  EXPECT_LT(map.load_factor(), 1.0F);

  // contains() — C++20 style existence check (absl provides this).
  EXPECT_TRUE(map.contains(0));
  EXPECT_TRUE(map.contains(499));
  EXPECT_FALSE(map.contains(500));
}

// =============================================================================
// 6. CustomHashWithAbslHash — AbslHashValue protocol for custom keys
// =============================================================================

namespace {

// Custom key type: an order identified by (symbol_id, sequence_number).
// To use this as a key in absl::flat_hash_map, we implement the AbslHashValue
// protocol: a free function template that Abseil's hashing framework discovers
// via ADL (Argument-Dependent Lookup).
//
// The AbslHashValue protocol is superior to specializing std::hash because:
//   1. It composes automatically — H::combine handles multi-field hashing.
//   2. It's type-safe — the hash state H is a template parameter, supporting
//      different hash algorithms without changing user code.
//   3. It avoids the "hash_combine" problem — the framework handles mixing,
//      so users don't need to manually XOR/shift/multiply partial hashes.
struct OrderKey {
  std::uint32_t symbol_id;
  std::uint64_t seq_num;

  bool operator==(const OrderKey &other) const = default;

  // AbslHashValue: Abseil calls this via ADL when hashing an OrderKey.
  // H is the hash state type (an opaque template parameter).
  // H::combine() feeds each field into the hash state in order.
  template <typename H>
  friend H AbslHashValue(H h, const OrderKey &key) { // NOLINT(readability-identifier-naming)
    return H::combine(std::move(h), key.symbol_id, key.seq_num);
  }
};

} // namespace

TEST(AbslFlatHashMapTest, CustomHashWithAbslHash) {
  absl::flat_hash_map<OrderKey, double> map;

  const OrderKey id1{.symbol_id = 1, .seq_num = 1000};
  const OrderKey id2{.symbol_id = 1, .seq_num = 1001};
  const OrderKey id3{.symbol_id = 2, .seq_num = 1000}; // Same seq_num as id1, different symbol.

  map[id1] = 99.50;
  map[id2] = 100.25;
  map[id3] = 50.00;

  EXPECT_EQ(map.size(), 3U);

  auto it1 = map.find(id1);
  ASSERT_NE(it1, map.end());
  EXPECT_DOUBLE_EQ(it1->second, 99.50);

  auto it2 = map.find(id2);
  ASSERT_NE(it2, map.end());
  EXPECT_DOUBLE_EQ(it2->second, 100.25);

  auto it3 = map.find(id3);
  ASSERT_NE(it3, map.end());
  EXPECT_DOUBLE_EQ(it3->second, 50.00);

  // Non-existent key.
  const OrderKey missing{.symbol_id = 3, .seq_num = 9999};
  EXPECT_EQ(map.find(missing), map.end());
}

// =============================================================================
// 7. CompareWithFixedHashMap — same dataset, results must match
// =============================================================================

TEST(AbslFlatHashMapTest, CompareWithFixedHashMap) {
  // Use the same dataset in both maps and verify results are identical.
  // FixedHashMap<K, V, 1024> can hold up to 1024 * 0.7 = 716 entries.
  constexpr std::size_t kCount = 500;

  mk::ds::FixedHashMap<std::uint64_t, std::uint64_t, 1024> fixed_map;
  absl::flat_hash_map<std::uint64_t, std::uint64_t> absl_map;
  absl_map.reserve(kCount);

  // Insert the same data into both maps.
  for (std::uint64_t i = 0; i < kCount; ++i) {
    const std::uint64_t key = (i * 7) + 13; // Non-sequential keys.
    const std::uint64_t val = i * 3;

    ASSERT_TRUE(fixed_map.insert(key, val))
        << "FixedHashMap insert failed at i=" << i;
    absl_map[key] = val;
  }

  EXPECT_EQ(fixed_map.size(), kCount);
  EXPECT_EQ(absl_map.size(), kCount);

  // Verify all lookups match.
  for (std::uint64_t i = 0; i < kCount; ++i) {
    const std::uint64_t key = (i * 7) + 13;

    auto *fixed_val = fixed_map.find(key);
    auto absl_it = absl_map.find(key);

    ASSERT_NE(fixed_val, nullptr) << "FixedHashMap missing key " << key;
    ASSERT_NE(absl_it, absl_map.end()) << "absl map missing key " << key;
    EXPECT_EQ(*fixed_val, absl_it->second) << "Value mismatch for key " << key;
  }

  // Verify non-existent keys are absent in both.
  EXPECT_EQ(fixed_map.find(0), nullptr);
  EXPECT_EQ(absl_map.find(0), absl_map.end());

  // Erase some keys from both and re-verify.
  for (std::uint64_t i = 0; i < 50; ++i) {
    const std::uint64_t key = (i * 7) + 13;
    EXPECT_TRUE(fixed_map.erase(key));
    EXPECT_EQ(absl_map.erase(key), 1U);
  }

  EXPECT_EQ(fixed_map.size(), kCount - 50);
  EXPECT_EQ(absl_map.size(), kCount - 50);

  // Erased keys should be absent in both.
  for (std::uint64_t i = 0; i < 50; ++i) {
    const std::uint64_t key = (i * 7) + 13;
    EXPECT_EQ(fixed_map.find(key), nullptr);
    EXPECT_EQ(absl_map.find(key), absl_map.end());
  }
}

// =============================================================================
// 8. LargeScaleInsertAndLookup — 100k insert + full lookup
// =============================================================================

TEST(AbslFlatHashMapTest, LargeScaleInsertAndLookup) {
  constexpr std::uint64_t kCount = 100'000;

  absl::flat_hash_map<std::uint64_t, std::uint64_t> map;
  map.reserve(kCount);

  // Insert 100k entries.
  for (std::uint64_t i = 0; i < kCount; ++i) {
    map[i] = i * 5;
  }
  EXPECT_EQ(map.size(), kCount);

  // Full lookup: every key must be present with the correct value.
  for (std::uint64_t i = 0; i < kCount; ++i) {
    auto it = map.find(i);
    ASSERT_NE(it, map.end()) << "Missing key " << i;
    EXPECT_EQ(it->second, i * 5) << "Wrong value for key " << i;
  }

  // Keys outside the range should not be found.
  EXPECT_EQ(map.find(kCount), map.end());
  EXPECT_EQ(map.find(kCount + 1000), map.end());

  // Erase all even keys.
  for (std::uint64_t i = 0; i < kCount; i += 2) {
    EXPECT_EQ(map.erase(i), 1U);
  }
  EXPECT_EQ(map.size(), kCount / 2);

  // Odd keys still present, even keys gone.
  for (std::uint64_t i = 0; i < kCount; ++i) {
    if (i % 2 == 0) {
      EXPECT_EQ(map.find(i), map.end())
          << "Even key " << i << " should be erased";
    } else {
      auto it = map.find(i);
      ASSERT_NE(it, map.end()) << "Odd key " << i << " should still exist";
      EXPECT_EQ(it->second, i * 5);
    }
  }
}
