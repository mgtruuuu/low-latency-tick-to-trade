/**
 * @file signal_logger_bench.cpp
 * @brief Microbenchmark for signal-safe logger operations.
 *
 * Measures the cost of formatting + write(2) for signal_log_to().
 * Writes to /dev/null to isolate formatting overhead from terminal I/O.
 *
 * Operations benchmarked:
 *   - signal_log_to (string)    — single string_view argument
 *   - signal_log_to (int)       — single integer argument
 *   - signal_log_to (mixed)     — string + int + float (typical log line)
 *   - Buf::put (string_view)    — raw buffer append (no write(2))
 *   - append<int64_t>           — to_chars integer formatting
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/signal_logger_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/signal_logger_bench
 */

#include "bench_utils.hpp"
#include "sys/log/signal_logger.hpp"
#include "sys/nano_clock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

using namespace mk::sys;
using namespace mk::sys::log;
using namespace mk::bench;

namespace {

// ============================================================================
// Configuration
// ============================================================================

constexpr std::size_t kN = 10'000;

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// Benchmarks
// ============================================================================

/// Measure Buf::put (string_view) — raw formatting without write(2).
void bench_buf_put_string(const TscCalibration &cal) {
  char storage[512];

  for (std::size_t i = 0; i < kN; ++i) {
    Buf buf{.data = storage, .cap = sizeof(storage), .pos = 0};
    const auto t0 = rdtsc_start();
    buf.put(std::string_view{"Order filled: price=12345 qty=100"});
    const auto t1 = rdtsc_end();
    do_not_optimize(buf.pos);
    g_latencies[i] = t1 - t0;
  }
  print_stats("Buf::put(sv)", compute_stats(cal, g_latencies));
}

/// Measure append<int64_t> — to_chars integer formatting.
void bench_append_int(const TscCalibration &cal) {
  char storage[512];

  for (std::size_t i = 0; i < kN; ++i) {
    Buf buf{.data = storage, .cap = sizeof(storage), .pos = 0};
    const auto val = static_cast<std::int64_t>(i * 12345);
    const auto t0 = rdtsc_start();
    append(buf, val);
    const auto t1 = rdtsc_end();
    do_not_optimize(buf.pos);
    g_latencies[i] = t1 - t0;
  }
  print_stats("append<int64>", compute_stats(cal, g_latencies));
}

/// Measure signal_log_to with a string — formatting + write(2) to /dev/null.
void bench_signal_log_string(const TscCalibration &cal, int fd) {
  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    signal_log_to(fd, std::string_view{"warmup\n"});
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    signal_log_to(fd, std::string_view{"Order filled: price=12345 qty=100\n"});
    const auto t1 = rdtsc_end();
    g_latencies[i] = t1 - t0;
  }
  print_stats("log_to(string)", compute_stats(cal, g_latencies));
}

/// Measure signal_log_to with integer — formatting + write(2) to /dev/null.
void bench_signal_log_int(const TscCalibration &cal, int fd) {
  for (std::size_t i = 0; i < kN; ++i) {
    const auto val = static_cast<std::int64_t>(i * 12345);
    const auto t0 = rdtsc_start();
    signal_log_to(fd, val, '\n');
    const auto t1 = rdtsc_end();
    g_latencies[i] = t1 - t0;
  }
  print_stats("log_to(int)", compute_stats(cal, g_latencies));
}

/// Measure signal_log_to with mixed args — typical log line.
void bench_signal_log_mixed(const TscCalibration &cal, int fd) {
  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    signal_log_to(fd, std::string_view{"FILL sym="}, 42,
                  std::string_view{" px="}, 12345,
                  std::string_view{" qty="}, 100, '\n');
    const auto t1 = rdtsc_end();
    g_latencies[i] = t1 - t0;
  }
  print_stats("log_to(mixed)", compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== SignalLogger Microbenchmark ===\n\n");

  // Open /dev/null to isolate formatting cost from terminal I/O.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const int devnull = ::open("/dev/null", O_WRONLY);
  if (devnull < 0) {
    std::printf("ERROR: cannot open /dev/null\n");
    return 1;
  }

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n", kN);
  std::printf("Output fd: /dev/null\n\n");

  std::printf("Buffer formatting (no write(2))\n");
  print_header();
  bench_buf_put_string(cal);
  bench_append_int(cal);

  std::printf("\nsignal_log_to() (formatting + write(2) to /dev/null)\n");
  print_header();
  bench_signal_log_string(cal, devnull);
  bench_signal_log_int(cal, devnull);
  bench_signal_log_mixed(cal, devnull);

  ::close(devnull);

  std::printf("\nTip: taskset -c N ./signal_logger_bench  for stable p99.\n");

  return 0;
}
