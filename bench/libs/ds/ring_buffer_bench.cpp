/**
 * @file ring_buffer_bench.cpp
 * @brief Microbenchmark for FixedRingBuffer and RingBuffer operations.
 *
 * Compares two ring buffer implementations:
 *   - FixedRingBuffer<T, N> — inline std::array storage, compile-time capacity
 *   - RingBuffer<T>         — caller-managed buffer, runtime capacity
 *
 * Operations benchmarked:
 *   - push          — into a half-full buffer
 *   - pop_front     — remove oldest element
 *   - push+pop      — round-trip (steady-state throughput)
 *   - operator[]    — random access by logical index
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/ds/ring_buffer_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/ds/ring_buffer_bench
 */

#include "bench_utils.hpp"
#include "ds/fixed_ring_buffer.hpp"
#include "ds/ring_buffer.hpp"
#include "sys/nano_clock.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

using namespace mk::sys;
using namespace mk::ds;
using namespace mk::bench;

namespace {

// ============================================================================
// Benchmark configuration
// ============================================================================

constexpr std::size_t kCapacity = 4096;
constexpr std::size_t kN = 10'000;

// Pre-fill to ~50% capacity for push benchmarks.
constexpr std::size_t kPrefillCount = kCapacity / 2;

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// FixedRingBuffer benchmarks
// ============================================================================

void bench_fixed_ring_buffer(const TscCalibration &cal) {
  // --- push ---
  {
    FixedRingBuffer<std::uint64_t, kCapacity> rb;
    for (std::size_t i = 0; i < kPrefillCount; ++i) {
      rb.push(i);
    }

    for (std::size_t i = 0; i < kN; ++i) {
      const auto t0 = rdtsc_start();
      rb.push(i);
      const auto t1 = rdtsc_end();
      g_latencies[i] = t1 - t0;
    }
    print_stats("Fixed: push", compute_stats(cal, g_latencies));
  }

  // --- pop_front ---
  {
    FixedRingBuffer<std::uint64_t, kCapacity> rb;
    for (std::size_t i = 0; i < kCapacity; ++i) {
      rb.push(i);
    }

    // Measure pop_front + re-push to keep the buffer non-empty.
    for (std::size_t i = 0; i < kN; ++i) {
      const auto t0 = rdtsc_start();
      rb.pop_front();
      const auto t1 = rdtsc_end();
      g_latencies[i] = t1 - t0;

      // Re-push to keep buffer populated for next iteration.
      rb.push(i);
    }
    print_stats("Fixed: pop_front", compute_stats(cal, g_latencies));
  }

  // --- push+pop round-trip ---
  {
    FixedRingBuffer<std::uint64_t, kCapacity> rb;
    for (std::size_t i = 0; i < kPrefillCount; ++i) {
      rb.push(i);
    }

    for (std::size_t i = 0; i < kN; ++i) {
      const auto t0 = rdtsc_start();
      rb.push(i);
      rb.pop_front();
      const auto t1 = rdtsc_end();
      g_latencies[i] = t1 - t0;
    }
    print_stats("Fixed: push+pop", compute_stats(cal, g_latencies));
  }

  // --- operator[] (random access) ---
  {
    FixedRingBuffer<std::uint64_t, kCapacity> rb;
    for (std::size_t i = 0; i < kCapacity; ++i) {
      rb.push(i);
    }

    for (std::size_t i = 0; i < kN; ++i) {
      // Access at varying indices to avoid trivial prefetch patterns.
      const auto idx = (i * 7) % kCapacity;
      const auto t0 = rdtsc_start();
      auto val = rb[idx];
      const auto t1 = rdtsc_end();
      do_not_optimize(val);
      g_latencies[i] = t1 - t0;
    }
    print_stats("Fixed: operator[]", compute_stats(cal, g_latencies));
  }
}

// ============================================================================
// RingBuffer benchmarks
// ============================================================================

void bench_ring_buffer(const TscCalibration &cal) {
  using RB = RingBuffer<std::uint64_t>;

  constexpr std::size_t kBufSize = RB::required_buffer_size(kCapacity);
  alignas(64) static std::byte buf_push[kBufSize];
  alignas(64) static std::byte buf_pop[kBufSize];
  alignas(64) static std::byte buf_roundtrip[kBufSize];
  alignas(64) static std::byte buf_access[kBufSize];

  // --- push ---
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rb = RB::create(buf_push, kBufSize, kCapacity).value();
    for (std::size_t i = 0; i < kPrefillCount; ++i) {
      rb.push(i);
    }

    for (std::size_t i = 0; i < kN; ++i) {
      const auto t0 = rdtsc_start();
      rb.push(i);
      const auto t1 = rdtsc_end();
      g_latencies[i] = t1 - t0;
    }
    print_stats("Runtime: push", compute_stats(cal, g_latencies));
  }

  // --- pop_front ---
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rb = RB::create(buf_pop, kBufSize, kCapacity).value();
    for (std::size_t i = 0; i < kCapacity; ++i) {
      rb.push(i);
    }

    for (std::size_t i = 0; i < kN; ++i) {
      const auto t0 = rdtsc_start();
      rb.pop_front();
      const auto t1 = rdtsc_end();
      g_latencies[i] = t1 - t0;

      rb.push(i);
    }
    print_stats("Runtime: pop_front", compute_stats(cal, g_latencies));
  }

  // --- push+pop round-trip ---
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rb = RB::create(buf_roundtrip, kBufSize, kCapacity).value();
    for (std::size_t i = 0; i < kPrefillCount; ++i) {
      rb.push(i);
    }

    for (std::size_t i = 0; i < kN; ++i) {
      const auto t0 = rdtsc_start();
      rb.push(i);
      rb.pop_front();
      const auto t1 = rdtsc_end();
      g_latencies[i] = t1 - t0;
    }
    print_stats("Runtime: push+pop", compute_stats(cal, g_latencies));
  }

  // --- operator[] (random access) ---
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto rb = RB::create(buf_access, kBufSize, kCapacity).value();
    for (std::size_t i = 0; i < kCapacity; ++i) {
      rb.push(i);
    }

    for (std::size_t i = 0; i < kN; ++i) {
      const auto idx = (i * 7) % kCapacity;
      const auto t0 = rdtsc_start();
      auto val = rb[idx];
      const auto t1 = rdtsc_end();
      do_not_optimize(val);
      g_latencies[i] = t1 - t0;
    }
    print_stats("Runtime: operator[]", compute_stats(cal, g_latencies));
  }
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== RingBuffer Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n", kN);
  std::printf("Buffer capacity: %zu  Pre-fill: %zu (~%.0f%%)\n\n", kCapacity,
              kPrefillCount,
              100.0 * static_cast<double>(kPrefillCount) /
                  static_cast<double>(kCapacity));

  std::printf("FixedRingBuffer<uint64_t, %zu>\n", kCapacity);
  print_header();
  bench_fixed_ring_buffer(cal);

  std::printf("\n");

  std::printf("RingBuffer<uint64_t> (capacity %zu)\n", kCapacity);
  print_header();
  bench_ring_buffer(cal);

  std::printf("\nTip: taskset -c N ./ring_buffer_bench  for stable p99.\n");

  return 0;
}
