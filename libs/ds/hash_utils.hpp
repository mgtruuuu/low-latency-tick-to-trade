/**
 * @file hash_utils.hpp
 * @brief Hash utilities for open-addressing hash maps.
 *
 * Provides:
 *   - mix64()            — MurmurHash3 finalizer (bit avalanche mixer)
 *   - SplitMix64         — Canonical SplitMix64 PRNG / sequential key mixer
 *   - finalize_hash()    — Final mix64 pass for composite key hashers
 *   - hash_combine_u64() — Boost-style golden-ratio seed combiner
 *   - fnv1a_hash()       — FNV-1a: deterministic byte-buffer hash
 *   - DefaultHash<Key>   — Integer-aware hasher (mix64 for ints, std::hash +
 *                          mix64 for others)
 *   - SlotState          — Open-addressing slot state (kEmpty/kFull/kTombstone)
 *
 * Why mix64 matters for open addressing:
 *   std::hash<int> on libstdc++ is the identity function (hash(5) == 5).
 *   With power-of-two table sizes, identity hashing causes catastrophic
 *   clustering because only the low bits select the bucket.
 *   mix64 provides full avalanche: every input bit affects every output bit,
 *   giving uniform distribution across all buckets.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional> // std::hash, std::equal_to
#include <span>
#include <type_traits>

namespace mk::ds {

// =============================================================================
// 1. mix64 — MurmurHash3 finalizer (fmix64)
// =============================================================================
//
// Bijective 64-bit → 64-bit mixer. Every input bit affects every output bit
// (full avalanche). Used by MurmurHash3 finalization and as a standalone
// avalanche mixer in hash tables and bloom-filter probe derivation.
//
// The magic constants and shift amounts were found by Austin Appleby through
// extensive statistical testing to minimize bias in the output distribution.
//
// Cost: 3 multiplies + 3 XOR-shifts ≈ ~4ns on modern x86.

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t k) noexcept {
  k ^= k >> 33;
  k *= 0xFF51AFD7ED558CCDULL;
  k ^= k >> 33;
  k *= 0xC4CEB9FE1A85EC53ULL;
  k ^= k >> 33;
  return k;
}

// =============================================================================
// 2. SplitMix64 — Canonical SplitMix64 PRNG / sequential key mixer
// =============================================================================
//
// Canonical SplitMix64 sequence:
//   state += gamma
//   z = state
//   z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
//   z = (z ^ (z >> 27)) * 0x94D049BB133111EB
//   z = z ^ (z >> 31)
//
// Used for two purposes:
//
//   1. **Bloom filter probe expansion**: produce k probe indices from a single
//      seed by calling next() repeatedly (single-seed PRNG expansion).
//      Each call advances the state and mixes, yielding independent-looking
//      indices without computing k separate hash functions.
//      Note: this is distinct from Kirsch–Mitzenmacher double-hashing
//      (h1 + i*h2), which uses two independent base hashes.
//
//   2. **Fast PRNG**: SplitMix64 passes BigCrush and
//      is the default seed mixer in java.util.SplittableRandom.
//
// Note: SplitMix64 uses a different output permutation from our mix64()
// helper above. The names are intentionally separate to avoid conflating the
// MurmurHash3 finalizer with canonical SplitMix64 constants.
//
// Cost: 1 add + 2 multiplies + 3 XOR-shifts ≈ a few ns on modern x86.

struct SplitMix64 {
  std::uint64_t state;

  explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state{seed} {}

  [[nodiscard]] constexpr std::uint64_t next() noexcept {
    state += 0x9E3779B97F4A7C15ULL; // gamma = 2^64 / phi

    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
};

// =============================================================================
// 3. finalize_hash — Final mix64 pass for composite key hashers
// =============================================================================
//
// Thin wrapper around mix64 for use at the end of a composite hash computation.
// hash_combine_u64 is optimised for speed, not maximum bit diffusion: it lacks
// full avalanche effect, so sequential inputs (sequence = 1, 2, 3…) can leave
// residual low-bit patterns. A final mix64 pass eliminates those patterns
// before the hash is used as a power-of-two bucket index.
//
// On 32-bit platforms size_t is 4 bytes: casting the 64-bit mix64 result back
// to size_t discards the upper half, defeating the purpose of mixing, so the
// else branch returns h unchanged. On Linux x86-64 the else branch is dead code.
//
// Usage:
//   size_t h = 0;
//   hash_combine_u64(h, std::hash<uint32_t>{}(id.exchange_id));
//   hash_combine_u64(h, std::hash<uint64_t>{}(id.sequence));
//   return finalize_hash(h);

[[nodiscard]] constexpr std::size_t finalize_hash(std::size_t h) noexcept {
  if constexpr (sizeof(std::size_t) == 8) {
    return static_cast<std::size_t>(mix64(static_cast<std::uint64_t>(h)));
  } else {
    return h;
  }
}

// =============================================================================
// 4. hash_combine_u64 — Boost golden-ratio combiner
// =============================================================================
//
// Combines a new hash value into an existing seed. The golden-ratio constant
// (2^64 / phi ≈ 0x9E3779B97F4A7C15) spreads bits to reduce correlation between
// successive combine calls.
//
// Usage pattern for composite keys:
//   std::size_t seed = 0;
//   hash_combine_u64(seed, hash(field1));
//   hash_combine_u64(seed, hash(field2));
//   return finalize_hash(seed);  // apply final mix64 pass

constexpr void hash_combine_u64(std::size_t &seed, std::size_t v) noexcept {
  seed ^= v + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2);
}

// =============================================================================
// 5. fnv1a_hash — FNV-1a deterministic byte-buffer hash
// =============================================================================
//
// Deterministic, cross-platform hash for short byte sequences (<= ~64 bytes).
// Useful for ticker symbols, instrument names, or any fixed-width byte keys
// where replay/debug determinism matters more than raw throughput.
//
// std::byte is enum class : unsigned char — sign extension is impossible, so
// the classic pitfall of casting signed char to uint64_t does not apply.
//
// Performance note: byte-at-a-time loop with no SIMD. Falls behind wyhash/
// xxHash for buffers > ~64 bytes. Prefer those for longer payloads.

[[nodiscard]] constexpr std::uint64_t
fnv1a_hash(std::span<const std::byte> data) noexcept {
  constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
  constexpr std::uint64_t kFnvPrime  = 1099511628211ULL;

  std::uint64_t h = kFnvOffset;
  for (const std::byte b : data) {
    h ^= static_cast<std::uint64_t>(b);
    h *= kFnvPrime;
  }
  return h;
}

// =============================================================================
// 6. DefaultHash<Key> — Integer-aware hasher
// =============================================================================
//
// For integral types: applies finalize_hash (avoids identity-hash problem).
// For other types: delegates to std::hash<Key>, then applies finalize_hash.
//
// This ensures good distribution regardless of the underlying std::hash
// quality, which is critical for power-of-two open-addressing tables.

template <class Key> struct DefaultHash {
  [[nodiscard]] constexpr std::size_t operator()(const Key &k) const noexcept {
    if constexpr (std::is_integral_v<Key>) {
      // Integer types: cast to size_t then finalize. This avoids the identity
      // hash that libstdc++ uses for std::hash<int>, which would cause
      // clustering with power-of-two table sizes.
      return finalize_hash(static_cast<std::size_t>(k));
    } else {
      // Non-integer types: delegate to std::hash, then finalize with mix64.
      // This improves distribution even if std::hash has poor avalanche.
      //
      // Enforce noexcept at compile time: if std::hash<Key> can throw,
      // our noexcept spec would silently call std::terminate — fail loud
      // at compile time instead.
      static_assert(std::is_nothrow_invocable_v<std::hash<Key>, const Key &>,
                    "DefaultHash requires std::hash<Key> to be noexcept");
      return finalize_hash(std::hash<Key>{}(k));
    }
  }
};

// =============================================================================
// 7. SlotState — open-addressing slot state
// =============================================================================
//
// Shared by FixedHashMap (inline storage) and HashMap (external buffer).
// uint8_t to minimize per-slot overhead (1 byte vs 4 for int enum).

enum class SlotState : std::uint8_t {
  kEmpty,     // Never occupied — terminates probe chains.
  kFull,      // Contains a live key-value pair.
  kTombstone, // Was occupied, now erased — probing continues past these.
};

} // namespace mk::ds
