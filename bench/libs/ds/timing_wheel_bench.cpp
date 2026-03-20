/**
 * @file timing_wheel_bench.cpp
 * @brief Microbenchmark for FixedTimingWheel and TimingWheel operations.
 *
 * Compares two timing wheel implementations:
 *   - FixedTimingWheel<WS, MT> — inline std::array storage, compile-time
 *     capacity
 *   - TimingWheel               — caller-managed buffer, runtime capacity
 *
 * Operations benchmarked:
 *   - schedule   — schedule a timer with random delay
 *   - cancel     — schedule then cancel (round-trip)
 *   - tick       — advance wheel, fire expired timers
 *
 * Uses xorshift64 PRNG for pseudo-random delays (NOT std::random —
 * deterministic, zero allocation, no syscalls).
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/ds/timing_wheel_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/ds/timing_wheel_bench
 */

#include "bench_utils.hpp"
#include "ds/fixed_timing_wheel.hpp"
#include "ds/timing_wheel.hpp"
#include "sys/nano_clock.hpp"
#include "sys/xorshift64.hpp"

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

constexpr std::size_t kWheelSize = 1024;
constexpr std::size_t kMaxTimers = 4096;
constexpr std::size_t kN = 10'000;

std::array<std::uint64_t, kN> g_latencies{};

/// No-op callback for timer fire — we only measure scheduling overhead.
void noop_callback(void * /*ctx*/) noexcept {}

// ============================================================================
// FixedTimingWheel benchmarks
// ============================================================================

void bench_fixed_timing_wheel(const TscCalibration &cal) {
  // --- schedule ---
  // Batched: after kBatch schedules, tick through all slots to clear timers,
  // then reset the wheel so it can accept new timers.
  {
    FixedTimingWheel<kWheelSize, kMaxTimers> wheel;
    Xorshift64 rng(0xDEAD'BEEF'CAFE'1234ULL);

    constexpr std::size_t kBatch = kMaxTimers / 2;
    std::size_t measured = 0;

    while (measured < kN) {
      const std::size_t count = std::min(kBatch, kN - measured);

      for (std::size_t i = 0; i < count; ++i) {
        const auto delay =
            static_cast<std::uint64_t>(1 + (rng() % (kWheelSize - 1)));
        const auto t0 = rdtsc_start();
        auto h = wheel.schedule(noop_callback, nullptr, delay);
        const auto t1 = rdtsc_end();
        do_not_optimize(h);
        g_latencies[measured + i] = t1 - t0;
      }

      // Clean up: tick through all slots to fire and free timers.
      for (std::size_t s = 0; s < kWheelSize; ++s) {
        wheel.tick();
      }
      measured += count;
    }
    print_stats("Fixed: schedule", compute_stats(cal, g_latencies));
  }

  // --- cancel ---
  // Schedule a timer, then immediately cancel it. Measures the schedule+cancel
  // round-trip minus scheduling overhead.
  {
    FixedTimingWheel<kWheelSize, kMaxTimers> wheel;
    Xorshift64 rng(0xDEAD'BEEF'CAFE'5678ULL);

    for (std::size_t i = 0; i < kN; ++i) {
      const auto delay =
          static_cast<std::uint64_t>(1 + (rng() % (kWheelSize - 1)));
      auto h = wheel.schedule(noop_callback, nullptr, delay);

      const auto t0 = rdtsc_start();
      auto ok = wheel.cancel(h);
      const auto t1 = rdtsc_end();
      do_not_optimize(ok);
      g_latencies[i] = t1 - t0;
    }
    print_stats("Fixed: cancel", compute_stats(cal, g_latencies));
  }

  // --- tick ---
  // Pre-schedule many timers spread across slots, then measure tick().
  // Each tick fires the timers in that slot (O(M) where M = timers in slot).
  {
    FixedTimingWheel<kWheelSize, kMaxTimers> wheel;
    Xorshift64 rng(0xDEAD'BEEF'CAFE'9ABCULL);

    // Fill ~75% of timer capacity, spread across slots.
    const std::size_t prefill = kMaxTimers * 3 / 4;
    for (std::size_t i = 0; i < prefill; ++i) {
      const auto delay =
          static_cast<std::uint64_t>(1 + (rng() % (kWheelSize - 1)));
      (void)wheel.schedule(noop_callback, nullptr, delay);
    }

    for (std::size_t i = 0; i < kN; ++i) {
      const auto t0 = rdtsc_start();
      auto fired = wheel.tick();
      const auto t1 = rdtsc_end();
      do_not_optimize(fired);
      g_latencies[i] = t1 - t0;

      // Re-schedule a timer to keep the wheel populated.
      const auto delay =
          static_cast<std::uint64_t>(1 + (rng() % (kWheelSize - 1)));
      (void)wheel.schedule(noop_callback, nullptr, delay);
    }
    print_stats("Fixed: tick", compute_stats(cal, g_latencies));
  }
}

// ============================================================================
// TimingWheel benchmarks
// ============================================================================

void bench_timing_wheel(const TscCalibration &cal) {
  constexpr std::size_t kBufSize =
      TimingWheel::required_buffer_size(kWheelSize, kMaxTimers);
  alignas(64) static std::byte buf_schedule[kBufSize];
  alignas(64) static std::byte buf_cancel[kBufSize];
  alignas(64) static std::byte buf_tick[kBufSize];

  // --- schedule ---
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto wheel = TimingWheel::create(buf_schedule, kBufSize, kWheelSize, kMaxTimers).value();
    Xorshift64 rng(0xDEAD'BEEF'CAFE'1234ULL);

    constexpr std::size_t kBatch = kMaxTimers / 2;
    std::size_t measured = 0;

    while (measured < kN) {
      const std::size_t count = std::min(kBatch, kN - measured);

      for (std::size_t i = 0; i < count; ++i) {
        const auto delay =
            static_cast<std::uint64_t>(1 + (rng() % (kWheelSize - 1)));
        const auto t0 = rdtsc_start();
        auto h = wheel.schedule(noop_callback, nullptr, delay);
        const auto t1 = rdtsc_end();
        do_not_optimize(h);
        g_latencies[measured + i] = t1 - t0;
      }

      for (std::size_t s = 0; s < kWheelSize; ++s) {
        wheel.tick();
      }
      measured += count;
    }
    print_stats("Runtime: schedule", compute_stats(cal, g_latencies));
  }

  // --- cancel ---
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto wheel = TimingWheel::create(buf_cancel, kBufSize, kWheelSize, kMaxTimers).value();
    Xorshift64 rng(0xDEAD'BEEF'CAFE'5678ULL);

    for (std::size_t i = 0; i < kN; ++i) {
      const auto delay =
          static_cast<std::uint64_t>(1 + (rng() % (kWheelSize - 1)));
      auto h = wheel.schedule(noop_callback, nullptr, delay);

      const auto t0 = rdtsc_start();
      auto ok = wheel.cancel(h);
      const auto t1 = rdtsc_end();
      do_not_optimize(ok);
      g_latencies[i] = t1 - t0;
    }
    print_stats("Runtime: cancel", compute_stats(cal, g_latencies));
  }

  // --- tick ---
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto wheel = TimingWheel::create(buf_tick, kBufSize, kWheelSize, kMaxTimers).value();
    Xorshift64 rng(0xDEAD'BEEF'CAFE'9ABCULL);

    const std::size_t prefill = kMaxTimers * 3 / 4;
    for (std::size_t i = 0; i < prefill; ++i) {
      const auto delay =
          static_cast<std::uint64_t>(1 + (rng() % (kWheelSize - 1)));
      (void)wheel.schedule(noop_callback, nullptr, delay);
    }

    for (std::size_t i = 0; i < kN; ++i) {
      const auto t0 = rdtsc_start();
      auto fired = wheel.tick();
      const auto t1 = rdtsc_end();
      do_not_optimize(fired);
      g_latencies[i] = t1 - t0;

      const auto delay =
          static_cast<std::uint64_t>(1 + (rng() % (kWheelSize - 1)));
      (void)wheel.schedule(noop_callback, nullptr, delay);
    }
    print_stats("Runtime: tick", compute_stats(cal, g_latencies));
  }
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== TimingWheel Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n", kN);
  std::printf("Wheel size: %zu  Max timers: %zu\n\n", kWheelSize, kMaxTimers);

  std::printf("FixedTimingWheel<%zu, %zu>\n", kWheelSize, kMaxTimers);
  print_header();
  bench_fixed_timing_wheel(cal);

  std::printf("\n");

  std::printf("TimingWheel (wheel_size %zu, max_timers %zu)\n", kWheelSize,
              kMaxTimers);
  print_header();
  bench_timing_wheel(cal);

  std::printf("\nTip: taskset -c N ./timing_wheel_bench  for stable p99.\n");

  return 0;
}
