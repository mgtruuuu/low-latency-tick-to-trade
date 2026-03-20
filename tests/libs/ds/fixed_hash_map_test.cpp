/**
 * @file fixed_hash_map_test.cpp
 * @brief GTest-based tests for FixedHashMap — fixed-capacity open-addressing
 * hash map.
 *
 * Test plan:
 *   1.  FindOnEmptyReturnsNull
 *   2.  InsertAndFind
 *   3.  InsertDuplicateReturnsFalse
 *   4.  UpsertOverwrites (size unchanged)
 *   5.  EraseAndFind
 *   6.  EraseNonexistentReturnsFalse
 *   7.  CollisionHandling (ConstantHash forcing all keys to bucket 0)
 *   8.  TombstoneProbeChain (erase middle of chain, find past it)
 *   9.  LoadFactorRejection (fill to 70%, verify insert fails but upsert of
 * existing succeeds)
 *   10. ClearResetsEverything
 *   11. NeedsRebuild (exceed 20% tombstones)
 *   12. CustomHasherAndKeyEqual (composite OrderId key with hash_combine_u64)
 *   13. TombstoneReuseOnInsert (tombstone_count decreases)
 *   14. ConstFind (const correctness)
 *   15. ManyInsertsThenLookups (500+ keys in 1024-cap map)
 */

#include "ds/fixed_hash_map.hpp"
#include "ds/hash_utils.hpp"

#include <gtest/gtest.h>

#include <cstdint>

// =============================================================================
// Test fixture with a small map (capacity = 16)
// =============================================================================

class FixedHashMapTest : public ::testing::Test {
protected:
  using Map = mk::ds::FixedHashMap<std::uint64_t, std::uint64_t, 16>;
  Map map_;
};

// =============================================================================
// 1. FindOnEmptyReturnsNull
// =============================================================================

TEST_F(FixedHashMapTest, FindOnEmptyReturnsNull) {
  EXPECT_EQ(nullptr, map_.find(42));
  EXPECT_TRUE(map_.empty());
  EXPECT_EQ(0U, map_.size());
}

// =============================================================================
// 2. InsertAndFind
// =============================================================================

TEST_F(FixedHashMapTest, InsertAndFind) {
  ASSERT_TRUE(map_.insert(10, 100));
  EXPECT_EQ(1U, map_.size());
  EXPECT_FALSE(map_.empty());

  auto *val = map_.find(10);
  ASSERT_NE(nullptr, val);
  EXPECT_EQ(100U, *val);
}

// =============================================================================
// 3. InsertDuplicateReturnsFalse
// =============================================================================

TEST_F(FixedHashMapTest, InsertDuplicateReturnsFalse) {
  ASSERT_TRUE(map_.insert(10, 100));
  EXPECT_FALSE(map_.insert(10, 200));
  EXPECT_EQ(1U, map_.size());

  // Value should be unchanged (insert-only, no overwrite).
  auto *val = map_.find(10);
  ASSERT_NE(nullptr, val);
  EXPECT_EQ(100U, *val);
}

// =============================================================================
// 4. UpsertOverwrites (size unchanged)
// =============================================================================

TEST_F(FixedHashMapTest, UpsertOverwrites) {
  using Result = Map::UpsertResult;

  // First upsert = insert.
  EXPECT_EQ(Result::kInserted, map_.upsert(10, 100));
  EXPECT_EQ(1U, map_.size());

  // Second upsert = update (value overwritten, size unchanged).
  EXPECT_EQ(Result::kUpdated, map_.upsert(10, 200));
  EXPECT_EQ(1U, map_.size());

  auto *val = map_.find(10);
  ASSERT_NE(nullptr, val);
  EXPECT_EQ(200U, *val);
}

// =============================================================================
// 5. EraseAndFind
// =============================================================================

TEST_F(FixedHashMapTest, EraseAndFind) {
  ASSERT_TRUE(map_.insert(10, 100));
  EXPECT_TRUE(map_.erase(10));
  EXPECT_EQ(0U, map_.size());
  EXPECT_EQ(nullptr, map_.find(10));
}

// =============================================================================
// 6. EraseNonexistentReturnsFalse
// =============================================================================

TEST_F(FixedHashMapTest, EraseNonexistentReturnsFalse) {
  EXPECT_FALSE(map_.erase(42));
  EXPECT_EQ(0U, map_.size());
}

// =============================================================================
// 7. CollisionHandling (ConstantHash forcing all keys to bucket 0)
// =============================================================================

namespace {

// A degenerate hash that maps every key to the same bucket.
// Forces maximum-length linear probe chains for collision testing.
struct ConstantHash {
  [[nodiscard]] std::size_t operator()(std::uint64_t /*unused*/) const noexcept {
    return 0;
  }
};

} // namespace

TEST(FixedHashMapCollision, AllKeysCollide) {
  mk::ds::FixedHashMap<std::uint64_t, std::uint64_t, 16, ConstantHash> map;

  // Insert several keys — they all hash to bucket 0 and probe linearly.
  for (std::uint64_t i = 0; i < 10; ++i) {
    ASSERT_TRUE(map.insert(i, i * 100));
  }
  EXPECT_EQ(10U, map.size());

  // All keys should be findable despite maximum collision.
  for (std::uint64_t i = 0; i < 10; ++i) {
    auto *val = map.find(i);
    ASSERT_NE(nullptr, val) << "Missing key " << i;
    EXPECT_EQ(i * 100, *val);
  }

  // Non-existent key should not be found.
  EXPECT_EQ(nullptr, map.find(999));
}

// =============================================================================
// 8. TombstoneProbeChain (erase middle of chain, find past it)
// =============================================================================

TEST(FixedHashMapCollision, TombstoneProbeChain) {
  mk::ds::FixedHashMap<std::uint64_t, std::uint64_t, 16, ConstantHash> map;

  // Insert keys 1, 2, 3 — all collide at bucket 0.
  // Physical layout: [1, 2, 3, empty, ...]
  ASSERT_TRUE(map.insert(1, 10));
  ASSERT_TRUE(map.insert(2, 20));
  ASSERT_TRUE(map.insert(3, 30));

  // Erase the middle key (2). This creates a tombstone.
  // Layout: [1, TOMBSTONE, 3, empty, ...]
  ASSERT_TRUE(map.erase(2));
  EXPECT_EQ(2U, map.size());
  EXPECT_EQ(1U, map.tombstone_count());

  // Key 3 is past the tombstone — probing must skip tombstones.
  auto *val3 = map.find(3);
  ASSERT_NE(nullptr, val3);
  EXPECT_EQ(30U, *val3);

  // Key 2 should not be found (it's erased).
  EXPECT_EQ(nullptr, map.find(2));

  // Key 1 should still be found.
  auto *val1 = map.find(1);
  ASSERT_NE(nullptr, val1);
  EXPECT_EQ(10U, *val1);
}

// =============================================================================
// 9. LoadFactorRejection
// =============================================================================

TEST_F(FixedHashMapTest, LoadFactorRejection) {
  // Capacity = 16, kMaxLoad = 16 * 7 / 10 = 11.
  // Insert 11 keys to reach the limit.
  for (std::uint64_t i = 0; i < 11; ++i) {
    ASSERT_TRUE(map_.insert(i, i * 10)) << "Insert failed at key " << i;
  }
  EXPECT_EQ(11U, map_.size());

  // 12th insert should fail (exceeds 70% load).
  EXPECT_FALSE(map_.insert(100, 1000));
  EXPECT_EQ(11U, map_.size());

  using Result = Map::UpsertResult;

  // Upsert of an EXISTING key should still succeed (updates don't increase
  // load).
  EXPECT_EQ(Result::kUpdated, map_.upsert(5, 999));
  auto *val = map_.find(5);
  ASSERT_NE(nullptr, val);
  EXPECT_EQ(999U, *val);
  EXPECT_EQ(11U, map_.size());

  // Upsert of a NEW key should fail with kCapacityFull (not kUpdated).
  EXPECT_EQ(Result::kCapacityFull, map_.upsert(200, 2000));
  EXPECT_EQ(nullptr, map_.find(200));
}

// =============================================================================
// 10. ClearResetsEverything
// =============================================================================

TEST_F(FixedHashMapTest, ClearResetsEverything) {
  for (std::uint64_t i = 0; i < 8; ++i) {
    ASSERT_TRUE(map_.insert(i, i * 10));
  }
  // Create some tombstones.
  ASSERT_TRUE(map_.erase(3));
  ASSERT_TRUE(map_.erase(5));

  map_.clear();
  EXPECT_EQ(0U, map_.size());
  EXPECT_EQ(0U, map_.tombstone_count());
  EXPECT_TRUE(map_.empty());

  // All keys should be gone.
  for (std::uint64_t i = 0; i < 8; ++i) {
    EXPECT_EQ(nullptr, map_.find(i));
  }

  // Should be able to insert again.
  ASSERT_TRUE(map_.insert(42, 420));
  EXPECT_EQ(1U, map_.size());
}

// =============================================================================
// 11. NeedsRebuild (exceed 20% tombstones)
// =============================================================================

TEST_F(FixedHashMapTest, NeedsRebuild) {
  // Capacity = 16, kTombstoneThreshold = 16 * 2 / 10 = 3.
  // needs_rebuild() returns true when tombstones > 3 (i.e., >= 4).

  EXPECT_FALSE(map_.needs_rebuild());

  // Insert 5 keys, then erase 4 to create 4 tombstones.
  for (std::uint64_t i = 0; i < 5; ++i) {
    ASSERT_TRUE(map_.insert(i, i * 10));
  }
  for (std::uint64_t i = 0; i < 4; ++i) {
    ASSERT_TRUE(map_.erase(i));
  }

  EXPECT_EQ(4U, map_.tombstone_count());
  EXPECT_TRUE(map_.needs_rebuild());
}

// =============================================================================
// 12. CustomHasherAndKeyEqual (composite OrderId key)
// =============================================================================

namespace {

// Composite key: an order identified by (symbol_id, sequence_number).
// Demonstrates hash_combine_u64 usage for multi-field keys.
struct OrderId {
  std::uint32_t symbol_id;
  std::uint64_t seq_num;

  bool operator==(const OrderId &other) const noexcept {
    return symbol_id == other.symbol_id && seq_num == other.seq_num;
  }
};

struct OrderIdHash {
  [[nodiscard]] std::size_t operator()(const OrderId &id) const noexcept {
    std::size_t seed = mk::ds::mix64(id.symbol_id);
    mk::ds::hash_combine_u64(seed, mk::ds::mix64(id.seq_num));
    return seed;
  }
};

} // namespace

TEST(FixedHashMapCustomKey, CompositeOrderIdKey) {
  mk::ds::FixedHashMap<OrderId, double, 64, OrderIdHash> map;

  const OrderId id1{.symbol_id = 1, .seq_num = 1000};
  const OrderId id2{.symbol_id = 1, .seq_num = 1001};
  const OrderId id3{.symbol_id = 2, .seq_num = 1000}; // Same seq_num as id1, different symbol.

  ASSERT_TRUE(map.insert(id1, 99.50));
  ASSERT_TRUE(map.insert(id2, 100.25));
  ASSERT_TRUE(map.insert(id3, 50.00));

  EXPECT_EQ(3U, map.size());

  auto *v1 = map.find(id1);
  ASSERT_NE(nullptr, v1);
  EXPECT_DOUBLE_EQ(99.50, *v1);

  auto *v2 = map.find(id2);
  ASSERT_NE(nullptr, v2);
  EXPECT_DOUBLE_EQ(100.25, *v2);

  auto *v3 = map.find(id3);
  ASSERT_NE(nullptr, v3);
  EXPECT_DOUBLE_EQ(50.00, *v3);

  // Non-existent key.
  const OrderId id_missing{.symbol_id = 3, .seq_num = 9999};
  EXPECT_EQ(nullptr, map.find(id_missing));
}

// =============================================================================
// 13. TombstoneReuseOnInsert (tombstone_count decreases)
// =============================================================================

TEST(FixedHashMapCollision, TombstoneReuseOnInsert) {
  mk::ds::FixedHashMap<std::uint64_t, std::uint64_t, 16, ConstantHash> map;

  // Insert keys 1, 2, 3 into the same collision chain.
  ASSERT_TRUE(map.insert(1, 10));
  ASSERT_TRUE(map.insert(2, 20));
  ASSERT_TRUE(map.insert(3, 30));

  // Erase key 2 → tombstone at position 1.
  ASSERT_TRUE(map.erase(2));
  EXPECT_EQ(1U, map.tombstone_count());
  EXPECT_EQ(2U, map.size());

  // Insert key 4. With ConstantHash, key 4 hashes to bucket 0.
  // Probing: slot 0 (key 1, full) → slot 1 (tombstone) → reuse tombstone.
  ASSERT_TRUE(map.insert(4, 40));
  EXPECT_EQ(0U, map.tombstone_count()); // Tombstone was reclaimed.
  EXPECT_EQ(3U, map.size());

  // Verify all live keys are findable.
  auto *v1 = map.find(1);
  ASSERT_NE(nullptr, v1);
  EXPECT_EQ(10U, *v1);

  auto *v4 = map.find(4);
  ASSERT_NE(nullptr, v4);
  EXPECT_EQ(40U, *v4);

  auto *v3 = map.find(3);
  ASSERT_NE(nullptr, v3);
  EXPECT_EQ(30U, *v3);

  // Key 2 is still erased.
  EXPECT_EQ(nullptr, map.find(2));
}

// =============================================================================
// 14. ConstFind (const correctness)
// =============================================================================

TEST_F(FixedHashMapTest, ConstFind) {
  ASSERT_TRUE(map_.insert(7, 77));

  // Access through const a reference.
  const auto &const_map = map_;
  const auto *val = const_map.find(7);
  ASSERT_NE(nullptr, val);
  EXPECT_EQ(77U, *val);

  // Non-existent key const on map.
  EXPECT_EQ(nullptr, const_map.find(999));
}

// =============================================================================
// 15. ManyInsertsThenLookups (500+ keys in 1024-cap map)
// =============================================================================

TEST(FixedHashMapLarge, ManyInsertsThenLookups) {
  // Capacity 1024, kMaxLoad = 1024 * 7 / 10 = 716.
  mk::ds::FixedHashMap<std::uint64_t, std::uint64_t, 1024> map;

  // Insert 700 keys (within load factor).
  constexpr std::uint64_t kCount = 700;
  for (std::uint64_t i = 0; i < kCount; ++i) {
    ASSERT_TRUE(map.insert(i, i * 3)) << "Insert failed at key " << i;
  }
  EXPECT_EQ(kCount, map.size());

  // All 700 keys must be found with correct values.
  for (std::uint64_t i = 0; i < kCount; ++i) {
    auto *val = map.find(i);
    ASSERT_NE(nullptr, val) << "Missing key " << i;
    EXPECT_EQ(i * 3, *val) << "Wrong value for key " << i;
  }

  // Keys above the range should not be found.
  EXPECT_EQ(nullptr, map.find(kCount));
  EXPECT_EQ(nullptr, map.find(kCount + 1000));
}

// =============================================================================
// 16. ForEachVisitsAllEntries
// =============================================================================

TEST(FixedHashMap, ForEachVisitsAllEntries) {
  mk::ds::FixedHashMap<std::uint64_t, std::uint64_t, 16> map;
  ASSERT_TRUE(map.insert(1, 10));
  ASSERT_TRUE(map.insert(2, 20));
  ASSERT_TRUE(map.insert(3, 30));
  ASSERT_TRUE(map.insert(4, 40));
  ASSERT_TRUE(map.insert(5, 50));

  std::uint64_t key_sum = 0;
  std::uint64_t val_sum = 0;
  std::size_t count = 0;
  auto visitor = [&](const std::uint64_t &k, const std::uint64_t &v) {
    key_sum += k;
    val_sum += v;
    ++count;
  };
  map.for_each(visitor);

  EXPECT_EQ(5U, count);
  EXPECT_EQ(1U + 2 + 3 + 4 + 5, key_sum);
  EXPECT_EQ(10U + 20 + 30 + 40 + 50, val_sum);
}

// =============================================================================
// 17. ForEachSkipsTombstones
// =============================================================================

TEST(FixedHashMap, ForEachSkipsTombstones) {
  mk::ds::FixedHashMap<std::uint64_t, std::uint64_t, 16> map;
  ASSERT_TRUE(map.insert(1, 10));
  ASSERT_TRUE(map.insert(2, 20));
  ASSERT_TRUE(map.insert(3, 30));

  // Erase key 2 — creates a tombstone.
  ASSERT_TRUE(map.erase(2));
  EXPECT_EQ(2U, map.size());

  std::size_t count = 0;
  std::uint64_t key_sum = 0;
  auto visitor = [&](const std::uint64_t &k, const std::uint64_t & /*v*/) {
    key_sum += k;
    ++count;
  };
  map.for_each(visitor);

  EXPECT_EQ(2U, count);
  EXPECT_EQ(1U + 3, key_sum);
}

// =============================================================================
// 18. ForEachOnEmpty
// =============================================================================

TEST(FixedHashMap, ForEachOnEmpty) {
  mk::ds::FixedHashMap<std::uint64_t, std::uint64_t, 16> map;
  std::size_t count = 0;
  auto visitor = [&](const std::uint64_t & /*k*/, const std::uint64_t & /*v*/) {
    ++count;
  };
  map.for_each(visitor);
  EXPECT_EQ(0U, count);
}

// =============================================================================
// 19. ConstForEach
// =============================================================================

TEST(FixedHashMap, ConstForEach) {
  mk::ds::FixedHashMap<std::uint64_t, std::uint64_t, 16> map;
  ASSERT_TRUE(map.insert(10, 100));
  ASSERT_TRUE(map.insert(20, 200));

  const auto &cmap = map;
  std::size_t count = 0;
  std::uint64_t val_sum = 0;
  auto visitor = [&](const std::uint64_t & /*k*/, const std::uint64_t &v) {
    val_sum += v;
    ++count;
  };
  cmap.for_each(visitor);

  EXPECT_EQ(2U, count);
  EXPECT_EQ(300U, val_sum);
}
