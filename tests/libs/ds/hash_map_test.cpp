/**
 * @file hash_map_test.cpp
 * @brief GTest-based tests for HashMap — runtime-capacity open-addressing
 * hash map with caller-managed storage.
 *
 * Test plan:
 *   Functional (mirror FixedHashMap tests):
 *     1.  FindOnEmptyReturnsNull
 *     2.  InsertAndFind
 *     3.  InsertDuplicateReturnsFalse
 *     4.  UpsertOverwrites (size unchanged)
 *     5.  EraseAndFind
 *     6.  EraseNonexistentReturnsFalse
 *     7.  CollisionHandling (ConstantHash forcing all keys to bucket 0)
 *     8.  TombstoneProbeChain (erase middle of chain, find past it)
 *     9.  LoadFactorRejection (fill to 70%, verify insert fails)
 *     10. ClearResetsEverything
 *     11. NeedsRebuild (exceed 20% tombstones)
 *     12. CustomHasherAndKeyEqual (composite OrderId key)
 *     13. TombstoneReuseOnInsert (tombstone_count decreases)
 *     14. ConstFind (const correctness)
 *     15. ManyInsertsThenLookups (500+ keys)
 *   Factory validation:
 *     16. FactoryWorks
 *     17. FactoryRejectsNull
 *     18. FactoryRejectsSmallBuffer
 *     19. FactoryRejectsMisaligned
 *   Move semantics:
 *     20. MoveConstructor
 *     21. MoveAssignment
 *   Static helpers:
 *     22. RoundUpCapacity
 *     23. RequiredBufferSize
 *     24. DefaultConstructedIsSafe
 *     25. MisalignedBufferAborts (death test)
 *     26. RoundUpCapacityOverflow
 */

#include "ds/hash_map.hpp"
#include "ds/hash_utils.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>

// =============================================================================
// Test fixture: creates a HashMap with capacity 16 using a stack buffer
// =============================================================================

class HashMapTest : public ::testing::Test {
protected:
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t>;
  static constexpr std::size_t kCap = 16;

  void SetUp() override {
    auto opt = Map::create(buf_, sizeof(buf_), kCap);
    ASSERT_TRUE(opt.has_value());
    map_ = std::move(*opt);  // NOLINT(bugprone-unchecked-optional-access)
  }

  // buf_ declared before map_ for correct initialization order.
  alignas(64) std::byte buf_[Map::required_buffer_size(kCap)]{};
  Map map_;
};

// =============================================================================
// 1. FindOnEmptyReturnsNull
// =============================================================================

TEST_F(HashMapTest, FindOnEmptyReturnsNull) {
  EXPECT_EQ(nullptr, map_.find(42));
  EXPECT_TRUE(map_.empty());
  EXPECT_EQ(0U, map_.size());
}

// =============================================================================
// 2. InsertAndFind
// =============================================================================

TEST_F(HashMapTest, InsertAndFind) {
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

TEST_F(HashMapTest, InsertDuplicateReturnsFalse) {
  ASSERT_TRUE(map_.insert(10, 100));
  EXPECT_FALSE(map_.insert(10, 200));
  EXPECT_EQ(1U, map_.size());

  auto *val = map_.find(10);
  ASSERT_NE(nullptr, val);
  EXPECT_EQ(100U, *val);
}

// =============================================================================
// 4. UpsertOverwrites
// =============================================================================

TEST_F(HashMapTest, UpsertOverwrites) {
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

TEST_F(HashMapTest, EraseAndFind) {
  ASSERT_TRUE(map_.insert(10, 100));
  EXPECT_TRUE(map_.erase(10));
  EXPECT_EQ(0U, map_.size());
  EXPECT_EQ(nullptr, map_.find(10));
}

// =============================================================================
// 6. EraseNonexistentReturnsFalse
// =============================================================================

TEST_F(HashMapTest, EraseNonexistentReturnsFalse) {
  EXPECT_FALSE(map_.erase(42));
  EXPECT_EQ(0U, map_.size());
}

// =============================================================================
// 7. CollisionHandling (ConstantHash forcing all keys to bucket 0)
// =============================================================================

namespace {

struct ConstantHash {
  [[nodiscard]] std::size_t operator()(std::uint64_t /*unused*/) const noexcept {
    return 0;
  }
};

} // namespace

TEST(HashMapCollision, AllKeysCollide) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t, ConstantHash>;
  constexpr std::size_t kCap = 16;
  alignas(64) std::byte buf[Map::required_buffer_size(kCap)]{};
  auto opt = Map::create(buf, sizeof(buf), kCap);
  ASSERT_TRUE(opt.has_value());
  auto &map = *opt;  // NOLINT(bugprone-unchecked-optional-access)

  for (std::uint64_t i = 0; i < 10; ++i) {
    ASSERT_TRUE(map.insert(i, i * 100));
  }
  EXPECT_EQ(10U, map.size());

  for (std::uint64_t i = 0; i < 10; ++i) {
    auto *val = map.find(i);
    ASSERT_NE(nullptr, val) << "Missing key " << i;
    EXPECT_EQ(i * 100, *val);
  }

  EXPECT_EQ(nullptr, map.find(999));
}

// =============================================================================
// 8. TombstoneProbeChain
// =============================================================================

TEST(HashMapCollision, TombstoneProbeChain) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t, ConstantHash>;
  constexpr std::size_t kCap = 16;
  alignas(64) std::byte buf[Map::required_buffer_size(kCap)]{};
  auto opt = Map::create(buf, sizeof(buf), kCap);
  ASSERT_TRUE(opt.has_value());
  auto &map = *opt;  // NOLINT(bugprone-unchecked-optional-access)

  ASSERT_TRUE(map.insert(1, 10));
  ASSERT_TRUE(map.insert(2, 20));
  ASSERT_TRUE(map.insert(3, 30));

  ASSERT_TRUE(map.erase(2));
  EXPECT_EQ(2U, map.size());
  EXPECT_EQ(1U, map.tombstone_count());

  auto *val3 = map.find(3);
  ASSERT_NE(nullptr, val3);
  EXPECT_EQ(30U, *val3);

  EXPECT_EQ(nullptr, map.find(2));

  auto *val1 = map.find(1);
  ASSERT_NE(nullptr, val1);
  EXPECT_EQ(10U, *val1);
}

// =============================================================================
// 9. LoadFactorRejection
// =============================================================================

TEST_F(HashMapTest, LoadFactorRejection) {
  // Capacity = 16, max_load = 16 * 7 / 10 = 11.
  for (std::uint64_t i = 0; i < 11; ++i) {
    ASSERT_TRUE(map_.insert(i, i * 10)) << "Insert failed at key " << i;
  }
  EXPECT_EQ(11U, map_.size());

  // 12th insert should fail.
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

TEST_F(HashMapTest, ClearResetsEverything) {
  for (std::uint64_t i = 0; i < 8; ++i) {
    ASSERT_TRUE(map_.insert(i, i * 10));
  }
  ASSERT_TRUE(map_.erase(3));
  ASSERT_TRUE(map_.erase(5));

  map_.clear();
  EXPECT_EQ(0U, map_.size());
  EXPECT_EQ(0U, map_.tombstone_count());
  EXPECT_TRUE(map_.empty());

  for (std::uint64_t i = 0; i < 8; ++i) {
    EXPECT_EQ(nullptr, map_.find(i));
  }

  ASSERT_TRUE(map_.insert(42, 420));
  EXPECT_EQ(1U, map_.size());
}

// =============================================================================
// 11. NeedsRebuild
// =============================================================================

TEST_F(HashMapTest, NeedsRebuild) {
  // Capacity = 16, tombstone_threshold = 16 * 2 / 10 = 3.
  EXPECT_FALSE(map_.needs_rebuild());

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
// 12. CustomHasherAndKeyEqual
// =============================================================================

namespace {

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

TEST(HashMapCustomKey, CompositeOrderIdKey) {
  using Map = mk::ds::HashMap<OrderId, double, OrderIdHash>;
  constexpr std::size_t kCap = 64;
  alignas(64) std::byte buf[Map::required_buffer_size(kCap)]{};
  auto opt = Map::create(buf, sizeof(buf), kCap);
  ASSERT_TRUE(opt.has_value());
  auto &map = *opt;  // NOLINT(bugprone-unchecked-optional-access)

  const OrderId id1{.symbol_id = 1, .seq_num = 1000};
  const OrderId id2{.symbol_id = 1, .seq_num = 1001};
  const OrderId id3{.symbol_id = 2, .seq_num = 1000};

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

  const OrderId id_missing{.symbol_id = 3, .seq_num = 9999};
  EXPECT_EQ(nullptr, map.find(id_missing));
}

// =============================================================================
// 13. TombstoneReuseOnInsert
// =============================================================================

TEST(HashMapCollision, TombstoneReuseOnInsert) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t, ConstantHash>;
  constexpr std::size_t kCap = 16;
  alignas(64) std::byte buf[Map::required_buffer_size(kCap)]{};
  auto opt = Map::create(buf, sizeof(buf), kCap);
  ASSERT_TRUE(opt.has_value());
  auto &map = *opt;  // NOLINT(bugprone-unchecked-optional-access)

  ASSERT_TRUE(map.insert(1, 10));
  ASSERT_TRUE(map.insert(2, 20));
  ASSERT_TRUE(map.insert(3, 30));

  ASSERT_TRUE(map.erase(2));
  EXPECT_EQ(1U, map.tombstone_count());
  EXPECT_EQ(2U, map.size());

  ASSERT_TRUE(map.insert(4, 40));
  EXPECT_EQ(0U, map.tombstone_count());
  EXPECT_EQ(3U, map.size());

  auto *v1 = map.find(1);
  ASSERT_NE(nullptr, v1);
  EXPECT_EQ(10U, *v1);

  auto *v4 = map.find(4);
  ASSERT_NE(nullptr, v4);
  EXPECT_EQ(40U, *v4);

  auto *v3 = map.find(3);
  ASSERT_NE(nullptr, v3);
  EXPECT_EQ(30U, *v3);

  EXPECT_EQ(nullptr, map.find(2));
}

// =============================================================================
// 14. ConstFind
// =============================================================================

TEST_F(HashMapTest, ConstFind) {
  ASSERT_TRUE(map_.insert(7, 77));

  const auto &const_map = map_;
  const auto *val = const_map.find(7);
  ASSERT_NE(nullptr, val);
  EXPECT_EQ(77U, *val);

  EXPECT_EQ(nullptr, const_map.find(999));
}

// =============================================================================
// 15. ManyInsertsThenLookups
// =============================================================================

TEST(HashMapLarge, ManyInsertsThenLookups) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t>;
  constexpr std::size_t kCap = 1024;
  alignas(64) std::byte buf[Map::required_buffer_size(kCap)]{};
  auto opt = Map::create(buf, sizeof(buf), kCap);
  ASSERT_TRUE(opt.has_value());
  auto &map = *opt;  // NOLINT(bugprone-unchecked-optional-access)

  // max_load = 1024 * 7 / 10 = 716. Insert 700.
  constexpr std::uint64_t kCount = 700;
  for (std::uint64_t i = 0; i < kCount; ++i) {
    ASSERT_TRUE(map.insert(i, i * 3)) << "Insert failed at key " << i;
  }
  EXPECT_EQ(kCount, map.size());

  for (std::uint64_t i = 0; i < kCount; ++i) {
    auto *val = map.find(i);
    ASSERT_NE(nullptr, val) << "Missing key " << i;
    EXPECT_EQ(i * 3, *val) << "Wrong value for key " << i;
  }

  EXPECT_EQ(nullptr, map.find(kCount));
  EXPECT_EQ(nullptr, map.find(kCount + 1000));
}

// =============================================================================
// 16. FactoryWorks
// =============================================================================

TEST(HashMapFactory, FactoryWorks) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t>;
  constexpr std::size_t kCap = 16;
  alignas(64) std::byte buf[Map::required_buffer_size(kCap)]{};
  auto opt = Map::create(buf, sizeof(buf), kCap);
  ASSERT_TRUE(opt.has_value());
  EXPECT_EQ(kCap, opt->capacity());   // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_TRUE(opt->empty());           // NOLINT(bugprone-unchecked-optional-access)

  ASSERT_TRUE(opt->insert(7, 77));     // NOLINT(bugprone-unchecked-optional-access)
  auto *val = opt->find(7);            // NOLINT(bugprone-unchecked-optional-access)
  ASSERT_NE(nullptr, val);
  EXPECT_EQ(77U, *val);
}

// =============================================================================
// 17. FactoryRejectsNull
// =============================================================================

TEST(HashMapFactory, FactoryRejectsNull) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t>;
  auto opt = Map::create(nullptr, 1024, 16);
  EXPECT_FALSE(opt.has_value());
}

// =============================================================================
// 18. FactoryRejectsSmallBuffer
// =============================================================================

TEST(HashMapFactory, FactoryRejectsSmallBuffer) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t>;
  alignas(64) std::byte buf[64]; // Way too small for 16 slots.
  auto opt = Map::create(buf, sizeof(buf), 16);
  EXPECT_FALSE(opt.has_value());
}

// =============================================================================
// 19. FactoryRejectsMisaligned
// =============================================================================

TEST(HashMapFactory, FactoryRejectsMisaligned) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t>;
  const std::size_t cap = 16;
  const std::size_t buf_size = Map::required_buffer_size(cap);

  // Allocate aligned buffer, then offset by 1 byte to guarantee misalignment.
  alignas(64) std::byte buf[1024];
  void *misaligned = buf + 1;
  auto opt = Map::create(misaligned, buf_size, cap);
  EXPECT_FALSE(opt.has_value());
}

// =============================================================================
// 20. MoveConstructor
// =============================================================================

TEST(HashMapMove, MoveConstructor) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t>;
  constexpr std::size_t kCap = 16;
  alignas(64) std::byte buf[Map::required_buffer_size(kCap)]{};
  auto opt = Map::create(buf, sizeof(buf), kCap);
  ASSERT_TRUE(opt.has_value());
  ASSERT_TRUE(opt->insert(1, 100));     // NOLINT(bugprone-unchecked-optional-access)
  ASSERT_TRUE(opt->insert(2, 200));     // NOLINT(bugprone-unchecked-optional-access)

  Map moved(std::move(*opt));           // NOLINT(bugprone-unchecked-optional-access)

  // Moved-to map has the data.
  EXPECT_EQ(kCap, moved.capacity());
  EXPECT_EQ(2U, moved.size());

  auto *v1 = moved.find(1);
  ASSERT_NE(nullptr, v1);
  EXPECT_EQ(100U, *v1);

  auto *v2 = moved.find(2);
  ASSERT_NE(nullptr, v2);
  EXPECT_EQ(200U, *v2);

  // Moved-from map is empty.
  EXPECT_EQ(0U, opt->capacity());  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(0U, opt->size());      // NOLINT(bugprone-unchecked-optional-access)
}

// =============================================================================
// 21. MoveAssignment
// =============================================================================

TEST(HashMapMove, MoveAssignment) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t>;
  constexpr std::size_t kCap = 16;
  alignas(64) std::byte buf[Map::required_buffer_size(kCap)]{};
  auto opt = Map::create(buf, sizeof(buf), kCap);
  ASSERT_TRUE(opt.has_value());
  ASSERT_TRUE(opt->insert(5, 500));  // NOLINT(bugprone-unchecked-optional-access)

  Map target;
  target = std::move(*opt);         // NOLINT(bugprone-unchecked-optional-access)

  EXPECT_EQ(kCap, target.capacity());
  EXPECT_EQ(1U, target.size());
  auto *val = target.find(5);
  ASSERT_NE(nullptr, val);
  EXPECT_EQ(500U, *val);

  // Moved-from is empty.
  EXPECT_EQ(0U, opt->capacity());  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(0U, opt->size());      // NOLINT(bugprone-unchecked-optional-access)
}

// =============================================================================
// 22. RoundUpCapacity
// =============================================================================

TEST(HashMapStatic, RoundUpCapacity) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t>;
  EXPECT_EQ(0U, Map::round_up_capacity(0));
  EXPECT_EQ(4U, Map::round_up_capacity(1));
  EXPECT_EQ(4U, Map::round_up_capacity(2));
  EXPECT_EQ(4U, Map::round_up_capacity(3));
  EXPECT_EQ(4U, Map::round_up_capacity(4));
  EXPECT_EQ(8U, Map::round_up_capacity(5));
  EXPECT_EQ(16U, Map::round_up_capacity(10));
  EXPECT_EQ(16U, Map::round_up_capacity(16));
  EXPECT_EQ(32U, Map::round_up_capacity(17));
  EXPECT_EQ(1024U, Map::round_up_capacity(1000));
}

// =============================================================================
// 23. RequiredBufferSize
// =============================================================================

TEST(HashMapStatic, RequiredBufferSize) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t>;
  EXPECT_EQ(16U * Map::slot_size(), Map::required_buffer_size(16));
  EXPECT_EQ(1024U * Map::slot_size(), Map::required_buffer_size(1024));
  EXPECT_GT(Map::slot_size(), 0U);
}

// =============================================================================
// 24. DefaultConstructedIsSafe
// =============================================================================

TEST(HashMapSpecial, DefaultConstructedIsSafe) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t>;
  Map map;
  EXPECT_EQ(0U, map.capacity());
  EXPECT_EQ(0U, map.size());
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(nullptr, map.find(123));
  EXPECT_EQ(nullptr, static_cast<const Map &>(map).find(123));
  EXPECT_FALSE(map.insert(1, 1));
  EXPECT_EQ(Map::UpsertResult::kCapacityFull, map.upsert(1, 1));
  EXPECT_FALSE(map.erase(1));
  map.clear(); // Should not crash.
  EXPECT_EQ(0U, map.size());
}

// =============================================================================
// 25. MisalignedBufferAborts (death test)
// =============================================================================

TEST(HashMapDeathTest, MisalignedBufferAborts) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t>;
  alignas(64) std::byte buf[1024];
  void *misaligned = buf + 1;
  EXPECT_DEATH(Map(misaligned, 1000, 16), "");
}

// =============================================================================
// 26. RoundUpCapacityOverflow
// =============================================================================

TEST(HashMapStatic, RoundUpCapacityOverflow) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t>;
  // Capacity beyond uint32_t max should return 0 (error sentinel).
  const auto too_large =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
  EXPECT_EQ(0U, Map::round_up_capacity(too_large));
}

// =============================================================================
// 27. ForEachVisitsAllEntries
// =============================================================================

TEST(HashMapForEach, ForEachVisitsAllEntries) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t>;
  constexpr std::size_t kCap = 16;
  alignas(64) std::byte buf[Map::required_buffer_size(kCap)]{};
  auto opt = Map::create(buf, sizeof(buf), kCap);
  ASSERT_TRUE(opt.has_value());
  auto &map = *opt; // NOLINT(bugprone-unchecked-optional-access)

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
// 28. ForEachSkipsTombstones
// =============================================================================

TEST(HashMapForEach, ForEachSkipsTombstones) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t>;
  constexpr std::size_t kCap = 16;
  alignas(64) std::byte buf[Map::required_buffer_size(kCap)]{};
  auto opt = Map::create(buf, sizeof(buf), kCap);
  ASSERT_TRUE(opt.has_value());
  auto &map = *opt; // NOLINT(bugprone-unchecked-optional-access)

  ASSERT_TRUE(map.insert(1, 10));
  ASSERT_TRUE(map.insert(2, 20));
  ASSERT_TRUE(map.insert(3, 30));
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
// 29. ForEachOnEmpty
// =============================================================================

TEST(HashMapForEach, ForEachOnEmpty) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t>;
  constexpr std::size_t kCap = 16;
  alignas(64) std::byte buf[Map::required_buffer_size(kCap)]{};
  auto opt = Map::create(buf, sizeof(buf), kCap);
  ASSERT_TRUE(opt.has_value());
  auto &map = *opt; // NOLINT(bugprone-unchecked-optional-access)

  std::size_t count = 0;
  auto visitor = [&](const std::uint64_t & /*k*/, const std::uint64_t & /*v*/) {
    ++count;
  };
  map.for_each(visitor);
  EXPECT_EQ(0U, count);
}

// =============================================================================
// 30. ConstForEach
// =============================================================================

TEST(HashMapForEach, ConstForEach) {
  using Map = mk::ds::HashMap<std::uint64_t, std::uint64_t>;
  constexpr std::size_t kCap = 16;
  alignas(64) std::byte buf[Map::required_buffer_size(kCap)]{};
  auto opt = Map::create(buf, sizeof(buf), kCap);
  ASSERT_TRUE(opt.has_value());
  auto &map = *opt; // NOLINT(bugprone-unchecked-optional-access)

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
