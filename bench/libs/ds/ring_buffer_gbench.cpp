/**
 * @file ring_buffer_gbench.cpp
 * @brief Google Benchmark version of RingBuffer benchmarks.
 *
 * Companion to ring_buffer_bench.cpp (custom rdtsc). This version uses
 * Google Benchmark for CI-friendly output, regression tracking, and JSON
 * export.
 *
 * Compares two ring buffer implementations:
 *   - FixedRingBuffer<T, N> — inline std::array storage
 *   - RingBuffer<T>         — caller-managed buffer, runtime capacity
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/ds/ring_buffer_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/ds/ring_buffer_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/ds/ring_buffer_gbench
 */

#include "ds/fixed_ring_buffer.hpp"
#include "ds/ring_buffer.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

using namespace mk::ds;

namespace {

// ============================================================================
// Configuration
// ============================================================================

constexpr std::size_t kCapacity = 4096;
constexpr std::size_t kPrefillCount = kCapacity / 2;

// ============================================================================
// FixedRingBuffer
// ============================================================================

// --- push ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Fixed_Push(benchmark::State &state) {
  FixedRingBuffer<std::uint64_t, kCapacity> rb;
  for (std::size_t i = 0; i < kPrefillCount; ++i) {
    rb.push(i);
  }

  std::uint64_t val = 0;
  for (auto _ : state) {
    rb.push(val);
    ++val;
  }
}
BENCHMARK(BM_Fixed_Push);

// --- pop_front ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Fixed_PopFront(benchmark::State &state) {
  FixedRingBuffer<std::uint64_t, kCapacity> rb;
  for (std::size_t i = 0; i < kCapacity; ++i) {
    rb.push(i);
  }

  std::uint64_t val = 0;
  for (auto _ : state) {
    rb.pop_front();
    // Re-push to keep buffer populated.
    rb.push(val);
    ++val;
  }
}
BENCHMARK(BM_Fixed_PopFront);

// --- push+pop round-trip ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Fixed_PushPop(benchmark::State &state) {
  FixedRingBuffer<std::uint64_t, kCapacity> rb;
  for (std::size_t i = 0; i < kPrefillCount; ++i) {
    rb.push(i);
  }

  std::uint64_t val = 0;
  for (auto _ : state) {
    rb.push(val);
    rb.pop_front();
    ++val;
  }
}
BENCHMARK(BM_Fixed_PushPop);

// --- operator[] ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Fixed_RandomAccess(benchmark::State &state) {
  FixedRingBuffer<std::uint64_t, kCapacity> rb;
  for (std::size_t i = 0; i < kCapacity; ++i) {
    rb.push(i);
  }

  std::size_t idx = 0;
  for (auto _ : state) {
    auto val = rb[(idx * 7) % kCapacity];
    benchmark::DoNotOptimize(val);
    ++idx;
  }
}
BENCHMARK(BM_Fixed_RandomAccess);

// ============================================================================
// RingBuffer (runtime capacity)
// ============================================================================

using RuntimeRB = RingBuffer<std::uint64_t>;
constexpr std::size_t kBufSize = RuntimeRB::required_buffer_size(kCapacity);

// --- push ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Runtime_Push(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize];
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto rb = RuntimeRB::create(buf, kBufSize, kCapacity).value();
  for (std::size_t i = 0; i < kPrefillCount; ++i) {
    rb.push(i);
  }

  std::uint64_t val = 0;
  for (auto _ : state) {
    rb.push(val);
    ++val;
  }
}
BENCHMARK(BM_Runtime_Push);

// --- pop_front ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Runtime_PopFront(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize];
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto rb = RuntimeRB::create(buf, kBufSize, kCapacity).value();
  for (std::size_t i = 0; i < kCapacity; ++i) {
    rb.push(i);
  }

  std::uint64_t val = 0;
  for (auto _ : state) {
    rb.pop_front();
    rb.push(val);
    ++val;
  }
}
BENCHMARK(BM_Runtime_PopFront);

// --- push+pop round-trip ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Runtime_PushPop(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize];
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto rb = RuntimeRB::create(buf, kBufSize, kCapacity).value();
  for (std::size_t i = 0; i < kPrefillCount; ++i) {
    rb.push(i);
  }

  std::uint64_t val = 0;
  for (auto _ : state) {
    rb.push(val);
    rb.pop_front();
    ++val;
  }
}
BENCHMARK(BM_Runtime_PushPop);

// --- operator[] ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Runtime_RandomAccess(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize];
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto rb = RuntimeRB::create(buf, kBufSize, kCapacity).value();
  for (std::size_t i = 0; i < kCapacity; ++i) {
    rb.push(i);
  }

  std::size_t idx = 0;
  for (auto _ : state) {
    auto val = rb[(idx * 7) % kCapacity];
    benchmark::DoNotOptimize(val);
    ++idx;
  }
}
BENCHMARK(BM_Runtime_RandomAccess);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
