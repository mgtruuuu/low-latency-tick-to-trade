/**
 * @file hash_utils_test.cpp
 * @brief GTest-based tests for hash_utils.hpp — mix64, SplitMix64,
 * hash_combine_u64, finalize_hash, fnv1a_hash, DefaultHash.
 *
 * Test plan:
 *   mix64:
 *     1. ZeroIsFixedPoint    — mix64(0) == 0 (known fixed point of fmix64)
 *     2. Bijective           — distinct inputs produce distinct outputs
 *     3. Constexpr           — usable at compile time
 *     4. SingleBitAvalanche  — flipping one input bit changes many output bits
 *
 *   SplitMix64:
 *     5. KnownSequence       — canonical SplitMix64 outputs for seed 0
 *     6. DifferentSeeds      — different seeds produce different sequences
 *     7. Constexpr           — usable at compile time
 *
 *   hash_combine_u64:
 *     8. OrderSensitive      — combine(a, b) != combine(b, a)
 *     9. SameValueNonZero    — combine(v, v) != 0 (unlike plain XOR)
 *
 *   finalize_hash:
 *    10. NonZeroInput        — finalize_hash(non-zero) != 0
 *    11. Constexpr           — usable at compile time
 *    12. DiffersFromInput    — finalize_hash(h) != h for structured inputs
 *
 *   fnv1a_hash:
 *    13. EmptySpan           — empty input returns FNV offset constant
 *    14. Deterministic       — same input always produces same output
 *    15. Distinct            — different byte sequences produce different
 * hashes
 *    16. Constexpr           — usable at compile time
 *
 *   DefaultHash:
 *    17. IntegralTypes       — works for int, uint64_t, int8_t
 *    18. NonIntegralType     — works for std::string_view (delegates to
 * std::hash)
 *    19. DifferentFromIdentity — hash(k) != k for small integers
 */

#include "ds/hash_utils.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_set>

// =============================================================================
// mix64 tests
// =============================================================================

TEST(Mix64Test, ZeroIsFixedPoint) {
  // 0 is a fixed point of fmix64: every XOR-shift step produces 0 when
  // the input is 0 (0 >> n == 0, 0 * c == 0, 0 ^ 0 == 0).
  // This is a known property, not a bug. In practice, key == 0 is rare
  // and still lands in a valid bucket (bucket 0).
  EXPECT_EQ(0U, mk::ds::mix64(0));

  // Non-zero inputs must produce non-zero outputs (bijectivity).
  EXPECT_NE(0U, mk::ds::mix64(1));
  EXPECT_NE(0U, mk::ds::mix64(UINT64_MAX));
}

TEST(Mix64Test, Bijective) {
  // A bijective function must produce unique outputs for unique inputs.
  // Test with 1000 sequential inputs — no collisions allowed.
  std::unordered_set<std::uint64_t> seen;
  constexpr std::uint64_t kCount = 1000;

  for (std::uint64_t i = 0; i < kCount; ++i) {
    auto [_, inserted] = seen.insert(mk::ds::mix64(i));
    EXPECT_TRUE(inserted) << "Collision at input " << i;
  }
  EXPECT_EQ(kCount, seen.size());
}

TEST(Mix64Test, Constexpr) {
  // mix64 is marked constexpr — verify it works at compile time.
  constexpr auto kH = mk::ds::mix64(42);
  static_assert(kH != 0, "mix64(42) should be non-zero at compile time");
  EXPECT_NE(0U, kH);
}

TEST(Mix64Test, SingleBitAvalanche) {
  // Avalanche property: flipping a single input bit should flip roughly
  // half the output bits (~32 out of 64). We test that at least 16 bits
  // differ (a very conservative lower bound) to catch degenerate mixers.
  constexpr std::uint64_t kBase = 0x123456789ABCDEF0ULL;
  const std::uint64_t base_hash = mk::ds::mix64(kBase);

  for (int bit = 0; bit < 64; ++bit) {
    const std::uint64_t flipped = kBase ^ (std::uint64_t{1} << bit);
    const std::uint64_t flipped_hash = mk::ds::mix64(flipped);

    // Count differing bits between the two hashes.
    const auto diff_bits = std::bitset<64>(base_hash ^ flipped_hash).count();

    EXPECT_GE(diff_bits, 16) << "Flipping input bit " << bit << " only changed "
                             << diff_bits << " output bits (expected >= 16)";
  }
}

// =============================================================================
// SplitMix64 tests
// =============================================================================

TEST(SplitMix64Test, KnownSequence) {
  mk::ds::SplitMix64 gen{0};

  EXPECT_EQ(0xE220A8397B1DCDAFULL, gen.next());
  EXPECT_EQ(0x6E789E6AA1B965F4ULL, gen.next());
  EXPECT_EQ(0x06C45D188009454FULL, gen.next());
}

TEST(SplitMix64Test, DifferentSeeds) {
  mk::ds::SplitMix64 gen_a{0x123456789ABCDEF0ULL};
  mk::ds::SplitMix64 gen_b{0x123456789ABCDEE0ULL};

  EXPECT_NE(gen_a.next(), gen_b.next());
}

TEST(SplitMix64Test, Constexpr) {
  constexpr std::uint64_t kFirst = [] {
    mk::ds::SplitMix64 gen{0};
    return gen.next();
  }();

  static_assert(kFirst == 0xE220A8397B1DCDAFULL,
                "SplitMix64 seed 0 first output must match canonical sequence");
  EXPECT_EQ(0xE220A8397B1DCDAFULL, kFirst);
}

// =============================================================================
// hash_combine_u64 tests
// =============================================================================

TEST(HashCombineTest, OrderSensitive) {
  // combine(seed, a) then combine(seed, b)  !=  combine(seed, b) then
  // combine(seed, a) This is critical for composite keys: {1, 2} must hash
  // differently from {2, 1}.
  std::size_t seed_ab = 0;
  mk::ds::hash_combine_u64(seed_ab, 111);
  mk::ds::hash_combine_u64(seed_ab, 222);

  std::size_t seed_ba = 0;
  mk::ds::hash_combine_u64(seed_ba, 222);
  mk::ds::hash_combine_u64(seed_ba, 111);

  EXPECT_NE(seed_ab, seed_ba);
}

TEST(HashCombineTest, SameValueNonZero) {
  // Plain XOR: v ^ v == 0. hash_combine must not have this problem.
  std::size_t seed = 0;
  mk::ds::hash_combine_u64(seed, 42);
  mk::ds::hash_combine_u64(seed, 42);

  EXPECT_NE(0U, seed);
}

// =============================================================================
// finalize_hash tests
// =============================================================================

TEST(FinalizeHashTest, NonZeroInput) {
  // finalize_hash is mix64 under the hood — non-zero inputs must stay non-zero.
  EXPECT_NE(0U, mk::ds::finalize_hash(1));
  EXPECT_NE(0U, mk::ds::finalize_hash(0xDEADBEEFULL));
}

TEST(FinalizeHashTest, Constexpr) {
  constexpr auto kH = mk::ds::finalize_hash(42);
  static_assert(kH != 0,
                "finalize_hash(42) should be non-zero at compile time");
  EXPECT_NE(0U, kH);
}

TEST(FinalizeHashTest, DiffersFromInput) {
  // Sequential inputs should map to unrelated outputs, not stay close together.
  // If finalize_hash were identity, sequential hash values would cluster in
  // a power-of-two table.
  for (std::size_t i = 1; i <= 64; ++i) {
    EXPECT_NE(i, mk::ds::finalize_hash(i))
        << "finalize_hash produced identity for input " << i;
  }
}

// =============================================================================
// fnv1a_hash tests
// =============================================================================

TEST(Fnv1aHashTest, EmptySpan) {
  // Empty input must return the FNV offset basis (14695981039346656037).
  // This is the defined behaviour of FNV-1a for a zero-length message.
  constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
  EXPECT_EQ(kFnvOffset, mk::ds::fnv1a_hash(std::span<const std::byte>{}));
}

TEST(Fnv1aHashTest, Deterministic) {
  // Same input must always produce the same output (cross-platform determinism
  // is the primary reason to choose FNV-1a over std::hash).
  const std::array<std::byte, 5> data{
      std::byte{'h'}, std::byte{'e'}, std::byte{'l'},
      std::byte{'l'}, std::byte{'o'},
  };
  const auto h1 = mk::ds::fnv1a_hash(data);
  const auto h2 = mk::ds::fnv1a_hash(data);
  EXPECT_EQ(h1, h2);
  EXPECT_NE(0U, h1);
}

TEST(Fnv1aHashTest, Distinct) {
  // Different byte sequences must (almost certainly) produce different hashes.
  const std::array<std::byte, 3> bytes_a{std::byte{'a'}, std::byte{'b'},
                                         std::byte{'c'}};
  const std::array<std::byte, 3> bytes_b{std::byte{'a'}, std::byte{'b'},
                                         std::byte{'d'}};
  const std::array<std::byte, 3> bytes_c{std::byte{'c'}, std::byte{'b'},
                                         std::byte{'a'}};

  EXPECT_NE(mk::ds::fnv1a_hash(bytes_a), mk::ds::fnv1a_hash(bytes_b));
  // Order sensitivity: "abc" != "cba"
  EXPECT_NE(mk::ds::fnv1a_hash(bytes_a), mk::ds::fnv1a_hash(bytes_c));
}

TEST(Fnv1aHashTest, Constexpr) {
  constexpr std::array<std::byte, 3> kData{
      std::byte{'H'},
      std::byte{'F'},
      std::byte{'T'},
  };
  constexpr auto kH = mk::ds::fnv1a_hash(kData);
  static_assert(kH != 0, "fnv1a_hash should be non-zero at compile time");
  EXPECT_NE(0U, kH);
}

// =============================================================================
// DefaultHash tests
// =============================================================================

TEST(DefaultHashTest, IntegralTypes) {
  mk::ds::DefaultHash<int> const hash_int;
  mk::ds::DefaultHash<std::uint64_t> const hash_u64;
  mk::ds::DefaultHash<std::int8_t> const hash_i8;

  // Just verify they compile, run, and produce non-trivial output.
  EXPECT_NE(0U, hash_int(1));
  EXPECT_NE(0U, hash_u64(1));
  EXPECT_NE(0U, hash_i8(1));

  // Different integer types with the same numeric value should produce
  // the same hash (they all go through mix64(static_cast<uint64_t>(k))).
  EXPECT_EQ(hash_int(7), hash_u64(7));
}

TEST(DefaultHashTest, NonIntegralType) {
  // std::string_view is not integral — DefaultHash should delegate to
  // std::hash then finalize with mix64. Prefer string_view over std::string
  // on hot paths (no heap allocation).
  mk::ds::DefaultHash<std::string_view> const hash_sv;

  auto h1 = hash_sv("hello");
  auto h2 = hash_sv("world");

  // Different strings should (almost certainly) produce different hashes.
  EXPECT_NE(h1, h2);
  // Non-zero output.
  EXPECT_NE(0U, h1);
}

TEST(DefaultHashTest, DifferentFromIdentity) {
  // On libstdc++, std::hash<int>()(5) == 5. DefaultHash must NOT be identity
  // because it applies mix64, which is crucial for power-of-two table sizing.
  mk::ds::DefaultHash<int> const h;

  for (int i = 1; i <= 100; ++i) {
    EXPECT_NE(static_cast<std::size_t>(i), h(i))
        << "DefaultHash produced identity for input " << i;
  }
}
