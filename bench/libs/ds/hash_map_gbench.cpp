/**
 * @file hash_map_gbench.cpp
 * @brief Google Benchmark version of HashMap benchmarks.
 *
 * Companion to hash_map_bench.cpp (custom rdtsc). This version uses Google
 * Benchmark for CI-friendly output, regression tracking, and JSON export.
 *
 * Compares two hash map implementations:
 *   - FixedHashMap<K, V, N> — inline std::array storage
 *   - HashMap<K, V>         — caller-managed buffer, runtime capacity
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/ds/hash_map_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/ds/hash_map_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/ds/hash_map_gbench
 */

#include "ds/fixed_hash_map.hpp"
#include "ds/hash_map.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>

using namespace mk::ds;

namespace {

// ============================================================================
// Configuration
// ============================================================================

constexpr std::size_t kMapCapacity = 4096;
constexpr std::size_t kPrefillCount = kMapCapacity / 2; // ~50% load factor

// ============================================================================
// FixedHashMap
// ============================================================================

// --- insert ---
// Batched: erase every kBatch iterations to stay under load factor limit.
// PauseTiming/ResumeTiming has ~500ns overhead per call, so we amortize
// it across kBatch iterations rather than calling it on every iteration.
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Fixed_Insert(benchmark::State &state) {
  constexpr std::size_t kBatch = 500;
  FixedHashMap<std::uint32_t, std::uint32_t, kMapCapacity> map;

  for (std::uint32_t i = 0; i < kPrefillCount; ++i) {
    (void)map.insert(i, i);
  }

  std::size_t idx = 0;
  auto key = static_cast<std::uint32_t>(kPrefillCount);
  for (auto _ : state) {
    (void)map.insert(key, key);
    ++key;
    ++idx;
    if (idx % kBatch == 0) {
      state.PauseTiming();
      for (std::size_t j = 0; j < kBatch; ++j) {
        (void)map.erase(key - static_cast<std::uint32_t>(kBatch) +
                        static_cast<std::uint32_t>(j));
      }
      state.ResumeTiming();
    }
  }
}
BENCHMARK(BM_Fixed_Insert);

// --- find (hit) ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Fixed_FindHit(benchmark::State &state) {
  FixedHashMap<std::uint32_t, std::uint32_t, kMapCapacity> map;

  for (std::uint32_t i = 0; i < kPrefillCount; ++i) {
    (void)map.insert(i, i);
  }

  std::uint32_t idx = 0;
  for (auto _ : state) {
    auto *val = map.find(idx % static_cast<std::uint32_t>(kPrefillCount));
    benchmark::DoNotOptimize(val);
    ++idx;
  }
}
BENCHMARK(BM_Fixed_FindHit);

// --- find (miss) ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Fixed_FindMiss(benchmark::State &state) {
  FixedHashMap<std::uint32_t, std::uint32_t, kMapCapacity> map;

  for (std::uint32_t i = 0; i < kPrefillCount; ++i) {
    (void)map.insert(i, i);
  }

  const auto miss_base = static_cast<std::uint32_t>(kPrefillCount + 100'000);
  std::uint32_t idx = 0;
  for (auto _ : state) {
    auto *val = map.find(miss_base + idx);
    benchmark::DoNotOptimize(val);
    ++idx;
  }
}
BENCHMARK(BM_Fixed_FindMiss);

// --- erase ---
// Batched: re-insert every kBatch iterations to keep the map populated.
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Fixed_Erase(benchmark::State &state) {
  constexpr std::size_t kBatch = 500;
  FixedHashMap<std::uint32_t, std::uint32_t, kMapCapacity> map;

  for (std::uint32_t i = 0; i < kPrefillCount; ++i) {
    (void)map.insert(i, i);
  }

  std::size_t idx = 0;
  for (auto _ : state) {
    const auto key = static_cast<std::uint32_t>(idx % kPrefillCount);
    (void)map.erase(key);
    ++idx;
    if (idx % kBatch == 0) {
      state.PauseTiming();
      for (std::size_t j = 0; j < kBatch; ++j) {
        const auto k =
            static_cast<std::uint32_t>((idx - kBatch + j) % kPrefillCount);
        (void)map.insert(k, k);
      }
      state.ResumeTiming();
    }
  }
}
BENCHMARK(BM_Fixed_Erase);

// ============================================================================
// HashMap (runtime capacity)
// ============================================================================

// Buffer for HashMap — avoids per-benchmark allocation.
using RuntimeMap = HashMap<std::uint32_t, std::uint32_t>;
constexpr std::size_t kBufSize = RuntimeMap::required_buffer_size(kMapCapacity);

// --- insert ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Runtime_Insert(benchmark::State &state) {
  constexpr std::size_t kBatch = 500;
  alignas(64) std::byte buf[kBufSize];
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto map = RuntimeMap::create(buf, kBufSize, kMapCapacity).value();

  for (std::uint32_t i = 0; i < kPrefillCount; ++i) {
    (void)map.insert(i, i);
  }

  std::size_t idx = 0;
  auto key = static_cast<std::uint32_t>(kPrefillCount);
  for (auto _ : state) {
    (void)map.insert(key, key);
    ++key;
    ++idx;
    if (idx % kBatch == 0) {
      state.PauseTiming();
      for (std::size_t j = 0; j < kBatch; ++j) {
        (void)map.erase(key - static_cast<std::uint32_t>(kBatch) +
                        static_cast<std::uint32_t>(j));
      }
      state.ResumeTiming();
    }
  }
}
BENCHMARK(BM_Runtime_Insert);

// --- find (hit) ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Runtime_FindHit(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize];
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto map = RuntimeMap::create(buf, kBufSize, kMapCapacity).value();

  for (std::uint32_t i = 0; i < kPrefillCount; ++i) {
    (void)map.insert(i, i);
  }

  std::uint32_t idx = 0;
  for (auto _ : state) {
    auto *val = map.find(idx % static_cast<std::uint32_t>(kPrefillCount));
    benchmark::DoNotOptimize(val);
    ++idx;
  }
}
BENCHMARK(BM_Runtime_FindHit);

// --- find (miss) ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Runtime_FindMiss(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize];
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto map = RuntimeMap::create(buf, kBufSize, kMapCapacity).value();

  for (std::uint32_t i = 0; i < kPrefillCount; ++i) {
    (void)map.insert(i, i);
  }

  const auto miss_base = static_cast<std::uint32_t>(kPrefillCount + 100'000);
  std::uint32_t idx = 0;
  for (auto _ : state) {
    auto *val = map.find(miss_base + idx);
    benchmark::DoNotOptimize(val);
    ++idx;
  }
}
BENCHMARK(BM_Runtime_FindMiss);

// --- erase ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Runtime_Erase(benchmark::State &state) {
  constexpr std::size_t kBatch = 500;
  alignas(64) std::byte buf[kBufSize];
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto map = RuntimeMap::create(buf, kBufSize, kMapCapacity).value();

  for (std::uint32_t i = 0; i < kPrefillCount; ++i) {
    (void)map.insert(i, i);
  }

  std::size_t idx = 0;
  for (auto _ : state) {
    const auto key = static_cast<std::uint32_t>(idx % kPrefillCount);
    (void)map.erase(key);
    ++idx;
    if (idx % kBatch == 0) {
      state.PauseTiming();
      for (std::size_t j = 0; j < kBatch; ++j) {
        const auto k =
            static_cast<std::uint32_t>((idx - kBatch + j) % kPrefillCount);
        (void)map.insert(k, k);
      }
      state.ResumeTiming();
    }
  }
}
BENCHMARK(BM_Runtime_Erase);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
