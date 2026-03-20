/**
 * @file hash_map_bench.cpp
 * @brief Microbenchmark for FixedHashMap and HashMap operations.
 *
 * Compares two hash map implementations:
 *   - FixedHashMap<K, V, N> — inline std::array storage, compile-time capacity
 *   - HashMap<K, V>         — caller-managed buffer, runtime capacity
 *
 * Operations benchmarked:
 *   - insert()    — into a partially loaded map (~50% load factor)
 *   - find() hit  — key exists
 *   - find() miss — key does not exist
 *   - erase()     — tombstone deletion
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/ds/hash_map_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/ds/hash_map_bench
 */

#include "bench_utils.hpp"
#include "ds/fixed_hash_map.hpp"
#include "ds/hash_map.hpp"
#include "sys/nano_clock.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace mk::sys;
using namespace mk::ds;
using namespace mk::bench;

namespace {

// ============================================================================
// Benchmark configuration
// ============================================================================

constexpr std::size_t kMapCapacity = 4096;
constexpr std::size_t kN = 10'000;

// Pre-fill to ~50% load factor. 70% is the max — 50% keeps us in the
// sweet spot where linear probing averages ~1.5 probes per lookup.
constexpr std::size_t kPrefillCount = kMapCapacity / 2;

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// FixedHashMap benchmarks
// ============================================================================

void bench_fixed_hash_map(const TscCalibration &cal) {
  // --- insert ---
  // Fresh map for each benchmark to avoid tombstone interference.
  {
    FixedHashMap<std::uint32_t, std::uint32_t, kMapCapacity> map;

    // Pre-fill to ~50%.
    for (std::uint32_t i = 0; i < kPrefillCount; ++i) {
      (void)map.insert(i, i);
    }

    // Measure: insert new keys beyond the pre-fill range.
    // Batched: erase after each batch to stay under load factor limit.
    constexpr std::size_t kBatch = 500;
    std::size_t measured = 0;

    while (measured < kN) {
      const std::size_t count = std::min(kBatch, kN - measured);
      const auto base = static_cast<std::uint32_t>(kPrefillCount + measured);

      for (std::size_t i = 0; i < count; ++i) {
        const auto key = base + static_cast<std::uint32_t>(i);
        const auto t0 = rdtsc_start();
        (void)map.insert(key, key);
        const auto t1 = rdtsc_end();
        g_latencies[measured + i] = t1 - t0;
      }

      // Clean up: erase the just-inserted keys.
      for (std::size_t i = 0; i < count; ++i) {
        (void)map.erase(base + static_cast<std::uint32_t>(i));
      }
      measured += count;
    }
    print_stats("Fixed: insert", compute_stats(cal, g_latencies));
  }

  // --- find (hit) ---
  {
    FixedHashMap<std::uint32_t, std::uint32_t, kMapCapacity> map;
    for (std::uint32_t i = 0; i < kPrefillCount; ++i) {
      (void)map.insert(i, i);
    }

    // Warm-up.
    for (std::uint32_t i = 0; i < 500; ++i) {
      do_not_optimize(*map.find(i % static_cast<std::uint32_t>(kPrefillCount)));
    }

    for (std::size_t i = 0; i < kN; ++i) {
      const auto key = static_cast<std::uint32_t>(i % kPrefillCount);
      const auto t0 = rdtsc_start();
      auto *val = map.find(key);
      const auto t1 = rdtsc_end();
      do_not_optimize(val);
      g_latencies[i] = t1 - t0;
    }
    print_stats("Fixed: find (hit)", compute_stats(cal, g_latencies));
  }

  // --- find (miss) ---
  {
    FixedHashMap<std::uint32_t, std::uint32_t, kMapCapacity> map;
    for (std::uint32_t i = 0; i < kPrefillCount; ++i) {
      (void)map.insert(i, i);
    }

    // Keys that don't exist: offset beyond pre-fill range.
    const auto miss_base = static_cast<std::uint32_t>(kPrefillCount + 100'000);

    for (std::size_t i = 0; i < kN; ++i) {
      const auto key = miss_base + static_cast<std::uint32_t>(i);
      const auto t0 = rdtsc_start();
      auto *val = map.find(key);
      const auto t1 = rdtsc_end();
      do_not_optimize(val);
      g_latencies[i] = t1 - t0;
    }
    print_stats("Fixed: find (miss)", compute_stats(cal, g_latencies));
  }

  // --- erase ---
  {
    FixedHashMap<std::uint32_t, std::uint32_t, kMapCapacity> map;
    for (std::uint32_t i = 0; i < kPrefillCount; ++i) {
      (void)map.insert(i, i);
    }

    // Measure: erase + re-insert to keep the map populated.
    for (std::size_t i = 0; i < kN; ++i) {
      const auto key = static_cast<std::uint32_t>(i % kPrefillCount);
      const auto t0 = rdtsc_start();
      (void)map.erase(key);
      const auto t1 = rdtsc_end();
      g_latencies[i] = t1 - t0;

      // Re-insert so the next erase has a valid key.
      (void)map.insert(key, key);
    }
    print_stats("Fixed: erase", compute_stats(cal, g_latencies));
  }
}

// ============================================================================
// HashMap benchmarks
// ============================================================================

void bench_hash_map(const TscCalibration &cal) {
  using Map = HashMap<std::uint32_t, std::uint32_t>;

  // Allocate external buffer for HashMap.
  constexpr std::size_t kBufSize = Map::required_buffer_size(kMapCapacity);
  alignas(64) static std::byte buf_insert[kBufSize];
  alignas(64) static std::byte buf_find_hit[kBufSize];
  alignas(64) static std::byte buf_find_miss[kBufSize];
  alignas(64) static std::byte buf_erase[kBufSize];

  // --- insert ---
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto map = Map::create(buf_insert, kBufSize, kMapCapacity).value();
    for (std::uint32_t i = 0; i < kPrefillCount; ++i) {
      (void)map.insert(i, i);
    }

    constexpr std::size_t kBatch = 500;
    std::size_t measured = 0;

    while (measured < kN) {
      const std::size_t count = std::min(kBatch, kN - measured);
      const auto base = static_cast<std::uint32_t>(kPrefillCount + measured);

      for (std::size_t i = 0; i < count; ++i) {
        const auto key = base + static_cast<std::uint32_t>(i);
        const auto t0 = rdtsc_start();
        (void)map.insert(key, key);
        const auto t1 = rdtsc_end();
        g_latencies[measured + i] = t1 - t0;
      }

      for (std::size_t i = 0; i < count; ++i) {
        (void)map.erase(base + static_cast<std::uint32_t>(i));
      }
      measured += count;
    }
    print_stats("Runtime: insert", compute_stats(cal, g_latencies));
  }

  // --- find (hit) ---
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto map = Map::create(buf_find_hit, kBufSize, kMapCapacity).value();
    for (std::uint32_t i = 0; i < kPrefillCount; ++i) {
      (void)map.insert(i, i);
    }

    for (std::uint32_t i = 0; i < 500; ++i) {
      do_not_optimize(*map.find(i % static_cast<std::uint32_t>(kPrefillCount)));
    }

    for (std::size_t i = 0; i < kN; ++i) {
      const auto key = static_cast<std::uint32_t>(i % kPrefillCount);
      const auto t0 = rdtsc_start();
      auto *val = map.find(key);
      const auto t1 = rdtsc_end();
      do_not_optimize(val);
      g_latencies[i] = t1 - t0;
    }
    print_stats("Runtime: find (hit)", compute_stats(cal, g_latencies));
  }

  // --- find (miss) ---
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto map = Map::create(buf_find_miss, kBufSize, kMapCapacity).value();
    for (std::uint32_t i = 0; i < kPrefillCount; ++i) {
      (void)map.insert(i, i);
    }

    const auto miss_base = static_cast<std::uint32_t>(kPrefillCount + 100'000);

    for (std::size_t i = 0; i < kN; ++i) {
      const auto key = miss_base + static_cast<std::uint32_t>(i);
      const auto t0 = rdtsc_start();
      auto *val = map.find(key);
      const auto t1 = rdtsc_end();
      do_not_optimize(val);
      g_latencies[i] = t1 - t0;
    }
    print_stats("Runtime: find (miss)", compute_stats(cal, g_latencies));
  }

  // --- erase ---
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto map = Map::create(buf_erase, kBufSize, kMapCapacity).value();
    for (std::uint32_t i = 0; i < kPrefillCount; ++i) {
      (void)map.insert(i, i);
    }

    for (std::size_t i = 0; i < kN; ++i) {
      const auto key = static_cast<std::uint32_t>(i % kPrefillCount);
      const auto t0 = rdtsc_start();
      (void)map.erase(key);
      const auto t1 = rdtsc_end();
      g_latencies[i] = t1 - t0;

      (void)map.insert(key, key);
    }
    print_stats("Runtime: erase", compute_stats(cal, g_latencies));
  }
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== HashMap Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n", kN);
  std::printf("Map capacity: %zu  Pre-fill: %zu (~%.0f%% load)\n\n",
              kMapCapacity, kPrefillCount,
              100.0 * static_cast<double>(kPrefillCount) /
                  static_cast<double>(kMapCapacity));

  std::printf("FixedHashMap<uint32_t, uint32_t, %zu>\n", kMapCapacity);
  print_header();
  bench_fixed_hash_map(cal);

  std::printf("\n");

  std::printf("HashMap<uint32_t, uint32_t> (capacity %zu)\n", kMapCapacity);
  print_header();
  bench_hash_map(cal);

  std::printf("\nTip: taskset -c N ./hash_map_bench  for stable p99.\n");

  return 0;
}
