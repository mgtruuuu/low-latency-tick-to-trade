/**
 * @file mmap_region_bench.cpp
 * @brief Microbenchmark for MmapRegion allocation/deallocation cost.
 *
 * Cold-path component — measures mmap/munmap syscall overhead:
 *   - allocate_anonymous(4KB)  — single page allocation
 *   - allocate_anonymous(2MB)  — large allocation (THP candidate)
 *
 * mmap is slow (~microseconds per call), so kN is smaller than hot-path
 * benchmarks. Each iteration allocates and immediately destroys a region.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/mmap_region_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/mmap_region_bench
 */

#include "bench_utils.hpp"
#include "sys/memory/mmap_region.hpp"
#include "sys/nano_clock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

using namespace mk::sys;
using namespace mk::sys::memory;
using namespace mk::bench;

namespace {

// ============================================================================
// Configuration
// ============================================================================

// Smaller iteration count — mmap is slow (~microseconds).
constexpr std::size_t kN = 1000;

constexpr std::size_t kSize4K = 4096;
constexpr std::size_t kSize2M = 2UL * 1024 * 1024;

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// Benchmarks
// ============================================================================

void bench_anonymous_4k(const TscCalibration &cal) {
  // Warm-up.
  for (int i = 0; i < 50; ++i) {
    auto r = MmapRegion::allocate_anonymous(kSize4K);
    do_not_optimize(r);
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    auto r = MmapRegion::allocate_anonymous(kSize4K);
    r.reset();
    const auto t1 = rdtsc_end();
    g_latencies[i] = t1 - t0;
  }
  print_stats("anonymous 4KB alloc+free", compute_stats(cal, g_latencies));
}

void bench_anonymous_2m(const TscCalibration &cal) {
  // Warm-up.
  for (int i = 0; i < 50; ++i) {
    auto r = MmapRegion::allocate_anonymous(kSize2M);
    do_not_optimize(r);
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    auto r = MmapRegion::allocate_anonymous(kSize2M);
    r.reset();
    const auto t1 = rdtsc_end();
    g_latencies[i] = t1 - t0;
  }
  print_stats("anonymous 2MB alloc+free", compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== MmapRegion Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n\n", kN);

  print_header();
  bench_anonymous_4k(cal);
  bench_anonymous_2m(cal);

  std::printf("\nNote: mmap is a cold-path syscall (~microseconds).\n");
  std::printf("Tip:  taskset -c N ./mmap_region_bench  for stable p99.\n");

  return 0;
}
