/**
 * @file timing_wheel_gbench.cpp
 * @brief Google Benchmark version of TimingWheel benchmarks.
 *
 * Companion to timing_wheel_bench.cpp (custom rdtsc). This version uses
 * Google Benchmark for CI-friendly output, regression tracking, and JSON
 * export.
 *
 * Compares two timing wheel implementations:
 *   - FixedTimingWheel<WS, MT> — inline std::array storage
 *   - TimingWheel               — caller-managed buffer, runtime capacity
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/ds/timing_wheel_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/ds/timing_wheel_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/ds/timing_wheel_gbench
 */

#include "ds/fixed_timing_wheel.hpp"
#include "ds/timing_wheel.hpp"
#include "sys/xorshift64.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

using namespace mk::ds;
using namespace mk::sys;

namespace {

// ============================================================================
// Configuration
// ============================================================================

constexpr std::size_t kWheelSize = 1024;
constexpr std::size_t kMaxTimers = 4096;

/// No-op callback for timer fire — we only measure scheduling overhead.
void noop_callback(void * /*ctx*/) noexcept {}

// ============================================================================
// FixedTimingWheel
// ============================================================================

// --- schedule ---
// Batched: after kBatch schedules, pause and tick through all slots to clear.
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Fixed_Schedule(benchmark::State &state) {
  constexpr std::size_t kBatch = kMaxTimers / 2;
  FixedTimingWheel<kWheelSize, kMaxTimers> wheel;
  Xorshift64 rng(0xDEAD'BEEF'CAFE'1234ULL);

  std::size_t idx = 0;
  for (auto _ : state) {
    const auto delay =
        static_cast<std::uint64_t>(1 + (rng() % (kWheelSize - 1)));
    auto h = wheel.schedule(noop_callback, nullptr, delay);
    benchmark::DoNotOptimize(h);
    ++idx;
    if (idx % kBatch == 0) {
      state.PauseTiming();
      for (std::size_t s = 0; s < kWheelSize; ++s) {
        wheel.tick();
      }
      state.ResumeTiming();
    }
  }
}
BENCHMARK(BM_Fixed_Schedule);

// --- cancel ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Fixed_Cancel(benchmark::State &state) {
  FixedTimingWheel<kWheelSize, kMaxTimers> wheel;
  Xorshift64 rng(0xDEAD'BEEF'CAFE'5678ULL);

  for (auto _ : state) {
    state.PauseTiming();
    const auto delay =
        static_cast<std::uint64_t>(1 + (rng() % (kWheelSize - 1)));
    auto h = wheel.schedule(noop_callback, nullptr, delay);
    state.ResumeTiming();

    auto ok = wheel.cancel(h);
    benchmark::DoNotOptimize(ok);
  }
}
BENCHMARK(BM_Fixed_Cancel);

// --- tick ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Fixed_Tick(benchmark::State &state) {
  FixedTimingWheel<kWheelSize, kMaxTimers> wheel;
  Xorshift64 rng(0xDEAD'BEEF'CAFE'9ABCULL);

  // Pre-fill ~75% of timer capacity.
  const std::size_t prefill = kMaxTimers * 3 / 4;
  for (std::size_t i = 0; i < prefill; ++i) {
    const auto delay =
        static_cast<std::uint64_t>(1 + (rng() % (kWheelSize - 1)));
    (void)wheel.schedule(noop_callback, nullptr, delay);
  }

  for (auto _ : state) {
    auto fired = wheel.tick();
    benchmark::DoNotOptimize(fired);

    // Re-schedule to keep the wheel populated.
    const auto delay =
        static_cast<std::uint64_t>(1 + (rng() % (kWheelSize - 1)));
    (void)wheel.schedule(noop_callback, nullptr, delay);
  }
}
BENCHMARK(BM_Fixed_Tick);

// ============================================================================
// TimingWheel (runtime capacity)
// ============================================================================

constexpr std::size_t kBufSize =
    TimingWheel::required_buffer_size(kWheelSize, kMaxTimers);

// --- schedule ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Runtime_Schedule(benchmark::State &state) {
  constexpr std::size_t kBatch = kMaxTimers / 2;
  alignas(64) std::byte buf[kBufSize];
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto wheel = TimingWheel::create(buf, kBufSize, kWheelSize, kMaxTimers).value();
  Xorshift64 rng(0xDEAD'BEEF'CAFE'1234ULL);

  std::size_t idx = 0;
  for (auto _ : state) {
    const auto delay =
        static_cast<std::uint64_t>(1 + (rng() % (kWheelSize - 1)));
    auto h = wheel.schedule(noop_callback, nullptr, delay);
    benchmark::DoNotOptimize(h);
    ++idx;
    if (idx % kBatch == 0) {
      state.PauseTiming();
      for (std::size_t s = 0; s < kWheelSize; ++s) {
        wheel.tick();
      }
      state.ResumeTiming();
    }
  }
}
BENCHMARK(BM_Runtime_Schedule);

// --- cancel ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Runtime_Cancel(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize];
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto wheel = TimingWheel::create(buf, kBufSize, kWheelSize, kMaxTimers).value();
  Xorshift64 rng(0xDEAD'BEEF'CAFE'5678ULL);

  for (auto _ : state) {
    state.PauseTiming();
    const auto delay =
        static_cast<std::uint64_t>(1 + (rng() % (kWheelSize - 1)));
    auto h = wheel.schedule(noop_callback, nullptr, delay);
    state.ResumeTiming();

    auto ok = wheel.cancel(h);
    benchmark::DoNotOptimize(ok);
  }
}
BENCHMARK(BM_Runtime_Cancel);

// --- tick ---
// NOLINTNEXTLINE(readability-identifier-naming)
void BM_Runtime_Tick(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize];
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto wheel = TimingWheel::create(buf, kBufSize, kWheelSize, kMaxTimers).value();
  Xorshift64 rng(0xDEAD'BEEF'CAFE'9ABCULL);

  const std::size_t prefill = kMaxTimers * 3 / 4;
  for (std::size_t i = 0; i < prefill; ++i) {
    const auto delay =
        static_cast<std::uint64_t>(1 + (rng() % (kWheelSize - 1)));
    (void)wheel.schedule(noop_callback, nullptr, delay);
  }

  for (auto _ : state) {
    auto fired = wheel.tick();
    benchmark::DoNotOptimize(fired);

    const auto delay =
        static_cast<std::uint64_t>(1 + (rng() % (kWheelSize - 1)));
    (void)wheel.schedule(noop_callback, nullptr, delay);
  }
}
BENCHMARK(BM_Runtime_Tick);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
