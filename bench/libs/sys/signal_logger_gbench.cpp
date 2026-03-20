/**
 * @file signal_logger_gbench.cpp
 * @brief Google Benchmark version of signal logger benchmarks.
 *
 * Companion to signal_logger_bench.cpp (custom rdtsc). This version uses
 * Google Benchmark for CI-friendly output, regression tracking, and JSON
 * export.
 *
 * Writes to /dev/null to isolate formatting overhead from terminal I/O.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/sys/signal_logger_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/sys/signal_logger_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/sys/signal_logger_gbench
 */

#include "sys/log/signal_logger.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

using namespace mk::sys::log;

namespace {

/// RAII wrapper for /dev/null fd.
struct DevNull {
  int fd;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  DevNull() : fd(::open("/dev/null", O_WRONLY)) {}
  ~DevNull() {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  DevNull(const DevNull &) = delete;
  DevNull &operator=(const DevNull &) = delete;
  DevNull(DevNull &&) = delete;
  DevNull &operator=(DevNull &&) = delete;
};

DevNull g_devnull; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// ============================================================================
// Buffer-only benchmarks (no write(2))
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_BufPutString(benchmark::State &state) {
  char storage[512];
  for (auto _ : state) {
    Buf buf{.data = storage, .cap = sizeof(storage), .pos = 0};
    buf.put(std::string_view{"Order filled: price=12345 qty=100"});
    benchmark::DoNotOptimize(buf.pos);
  }
}
BENCHMARK(BM_BufPutString);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_AppendInt64(benchmark::State &state) {
  char storage[512];
  std::int64_t val = 123456789;
  for (auto _ : state) {
    Buf buf{.data = storage, .cap = sizeof(storage), .pos = 0};
    append(buf, val);
    benchmark::DoNotOptimize(buf.pos);
    ++val;
  }
}
BENCHMARK(BM_AppendInt64);

// ============================================================================
// signal_log_to benchmarks (formatting + write(2) to /dev/null)
// ============================================================================

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_SignalLogString(benchmark::State &state) {
  for (auto _ : state) {
    signal_log_to(g_devnull.fd,
                  std::string_view{"Order filled: price=12345 qty=100\n"});
  }
}
BENCHMARK(BM_SignalLogString);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_SignalLogInt(benchmark::State &state) {
  std::int64_t val = 123456789;
  for (auto _ : state) {
    signal_log_to(g_devnull.fd, val, '\n');
    ++val;
  }
}
BENCHMARK(BM_SignalLogInt);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_SignalLogMixed(benchmark::State &state) {
  for (auto _ : state) {
    signal_log_to(g_devnull.fd, std::string_view{"FILL sym="}, 42,
                  std::string_view{" px="}, 12345,
                  std::string_view{" qty="}, 100, '\n');
  }
}
BENCHMARK(BM_SignalLogMixed);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
