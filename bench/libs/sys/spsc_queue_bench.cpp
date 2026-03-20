/**
 * @file spsc_queue_bench.cpp
 * @brief Microbenchmark for SPSC queue operations.
 *
 * Compares two SPSC queue implementations:
 *   - FixedSPSCQueue<T, N> — compile-time capacity, inline std::array storage
 *   - SPSCQueue<T>         — runtime capacity, MmapRegion-backed
 *
 * Single-threaded: measures raw try_push/try_pop latency, not concurrent
 * throughput. Both producer and consumer ops run on the same thread.
 * This isolates the cost of the atomic load/store + buffer write/read.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/spsc_queue_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/spsc_queue_bench
 */

#include "bench_utils.hpp"
#include "sys/memory/fixed_spsc_queue.hpp"
#include "sys/memory/mmap_utils.hpp"
#include "sys/memory/spsc_queue.hpp"
#include "sys/nano_clock.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

using namespace mk::sys;
using namespace mk::sys::memory;
using namespace mk::bench;

namespace {

// ============================================================================
// Benchmark configuration
// ============================================================================

constexpr std::uint32_t kQueueCapacity = 4096;
constexpr std::size_t kN = 10'000;

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// Generic queue benchmarks (work with both FixedSPSCQueue and SPSCQueue)
// ============================================================================

/// Benchmark try_push() into a half-full queue.
template <typename Queue>
void bench_push(Queue &q, const TscCalibration &cal, const char *label) {
  // Pre-fill to ~50% for realistic conditions.
  for (std::uint32_t i = 0; i < kQueueCapacity / 2; ++i) {
    (void)q.try_push(static_cast<std::uint64_t>(i));
  }

  // Warm-up: push/pop cycles to prime caches.
  for (int i = 0; i < 500; ++i) {
    std::uint64_t tmp = 0;
    (void)q.try_push(static_cast<std::uint64_t>(i));
    (void)q.try_pop(tmp);
  }

  // Measure: push one, then pop one to keep queue at ~50%.
  for (std::size_t i = 0; i < kN; ++i) {
    const auto val = static_cast<std::uint64_t>(i);

    const auto t0 = rdtsc_start();
    (void)q.try_push(val);
    const auto t1 = rdtsc_end();
    g_latencies[i] = t1 - t0;

    std::uint64_t tmp = 0;
    (void)q.try_pop(tmp);
  }

  // Drain remaining.
  std::uint64_t drain = 0;
  while (q.try_pop(drain)) {
  }

  print_stats(label, compute_stats(cal, g_latencies));
}

/// Benchmark try_pop() from a half-full queue.
template <typename Queue>
void bench_pop(Queue &q, const TscCalibration &cal, const char *label) {
  // Pre-fill to ~50%.
  for (std::uint32_t i = 0; i < kQueueCapacity / 2; ++i) {
    (void)q.try_push(static_cast<std::uint64_t>(i));
  }

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    std::uint64_t tmp = 0;
    (void)q.try_push(static_cast<std::uint64_t>(i));
    (void)q.try_pop(tmp);
  }

  // Measure: pop one, then push one to keep queue at ~50%.
  for (std::size_t i = 0; i < kN; ++i) {
    std::uint64_t tmp = 0;

    const auto t0 = rdtsc_start();
    (void)q.try_pop(tmp);
    const auto t1 = rdtsc_end();
    g_latencies[i] = t1 - t0;

    do_not_optimize(tmp);
    (void)q.try_push(static_cast<std::uint64_t>(i));
  }

  // Drain remaining.
  std::uint64_t drain = 0;
  while (q.try_pop(drain)) {
  }

  print_stats(label, compute_stats(cal, g_latencies));
}

/// Benchmark push + pop round-trip.
template <typename Queue>
void bench_round_trip(Queue &q, const TscCalibration &cal,
                      const char *label) {
  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    std::uint64_t tmp = 0;
    (void)q.try_push(static_cast<std::uint64_t>(i));
    (void)q.try_pop(tmp);
  }

  for (std::size_t i = 0; i < kN; ++i) {
    std::uint64_t tmp = 0;

    const auto t0 = rdtsc_start();
    (void)q.try_push(static_cast<std::uint64_t>(i));
    (void)q.try_pop(tmp);
    const auto t1 = rdtsc_end();
    g_latencies[i] = t1 - t0;

    do_not_optimize(tmp);
  }

  print_stats(label, compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== SPSC Queue Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n\n", kN);

  // --- FixedSPSCQueue (compile-time capacity, inline storage) ---
  std::printf("FixedSPSCQueue<uint64_t, %u>\n", kQueueCapacity);
  print_header();
  {
    FixedSPSCQueue<std::uint64_t, kQueueCapacity> q;
    bench_push(q, cal, "try_push");
    bench_pop(q, cal, "try_pop");
    bench_round_trip(q, cal, "push + pop round-trip");
  }

  std::printf("\n");

  // --- SPSCQueue (runtime capacity, MmapRegion-backed) ---
  std::printf("SPSCQueue<uint64_t> (capacity %u)\n", kQueueCapacity);
  print_header();
  {
    auto region = allocate_hot_rw_region(
        {.size = SPSCQueue<std::uint64_t>::required_buffer_size(kQueueCapacity)});
    SPSCQueue<std::uint64_t> q(static_cast<std::uint64_t *>(region.get()),
                                kQueueCapacity);
    bench_push(q, cal, "try_push");
    bench_pop(q, cal, "try_pop");
    bench_round_trip(q, cal, "push + pop round-trip");
  }

  std::printf("\nTip: taskset -c N ./spsc_queue_bench  for stable p99.\n");

  return 0;
}
