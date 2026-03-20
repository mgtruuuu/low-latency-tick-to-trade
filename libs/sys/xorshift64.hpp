/**
 * @file xorshift64.hpp
 * @brief Fast, non-cryptographic PRNG — xorshift64 (Marsaglia, 2003).
 *
 * Period: 2^64 - 1 (all states except 0). Shift triple (13, 7, 17).
 * Zero allocation, no syscalls, deterministic — suitable for hot-path
 * synthetic data generation, benchmarks, and tests.
 *
 * NOT suitable for: cryptography, security, or any use requiring
 * statistical uniformity guarantees (use std::mt19937 for that).
 *
 * Reference: George Marsaglia, "Xorshift RNGs", Journal of Statistical
 * Software, Vol. 8, Issue 14 (2003).
 */

#pragma once

#include <cstdint>
#include <cstdlib> // std::abort

namespace mk::sys {

class Xorshift64 {
public:
  /// Construct with explicit seed. Seed must not be 0 — state 0 is absorbing
  /// (0 XOR 0 = 0 forever). Aborts on zero seed.
  explicit constexpr Xorshift64(std::uint64_t seed) noexcept : state_(seed) {
    if (seed == 0) {
      std::abort();
    }
  }

  /// Generate next pseudo-random value.
  ///
  /// The three XOR-shift operations form a full-period generator over
  /// the non-zero 64-bit state space. The shift triple (13, 7, 17)
  /// is one of the triples listed by Marsaglia that produces a
  /// maximal-period sequence (2^64 - 1).
  [[nodiscard]] constexpr std::uint64_t operator()() noexcept {
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 7U;
    state_ ^= state_ << 17U;
    return state_;
  }

  /// Current internal state (for serialization / reproducibility).
  [[nodiscard]] constexpr std::uint64_t state() const noexcept {
    return state_;
  }

private:
  std::uint64_t state_;
};

} // namespace mk::sys
