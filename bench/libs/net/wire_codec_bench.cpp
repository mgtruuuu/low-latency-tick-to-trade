/**
 * @file wire_codec_bench.cpp
 * @brief Microbenchmark for WireWriter and WireReader operations.
 *
 * Measures per-field serialization/deserialization cost with bounds-checked
 * cursor and endian conversion. Simulates a typical order-entry message:
 *   [u64 order_id][u32 price][u32 qty][u16 symbol][u16 side]
 *
 * Operations benchmarked:
 *   - WireWriter: write 4 fields (u64 + u32 + u32 + u16 + u16 = 20 bytes)
 *   - WireReader: read 4 fields (same layout)
 *   - Round-trip: write then read (serialize + deserialize)
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/net/wire_codec_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/net/wire_codec_bench
 */

#include "bench_utils.hpp"
#include "net/wire_reader.hpp"
#include "net/wire_writer.hpp"
#include "sys/nano_clock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

using namespace mk::sys;
using namespace mk::net;
using namespace mk::bench;

namespace {

// ============================================================================
// Configuration
// ============================================================================

constexpr std::size_t kN = 10'000;
constexpr std::size_t kBufSize = 64;

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// Benchmarks
// ============================================================================

void bench_wire_writer(const TscCalibration &cal) {
  alignas(64) std::byte buf[kBufSize]{};

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    WireWriter w{.buf = std::span{buf}};
    w.write_u64_be(0xDEADBEEF);
    w.write_u32_be(12345);
    w.write_u32_be(100);
    w.write_u16_be(1);
    w.write_u16_be(0);
    do_not_optimize(w.pos);
  }

  for (std::size_t i = 0; i < kN; ++i) {
    WireWriter w{.buf = std::span{buf}};
    const auto t0 = rdtsc_start();
    w.write_u64_be(0xDEADBEEF + i);
    w.write_u32_be(12345);
    w.write_u32_be(100);
    w.write_u16_be(1);
    w.write_u16_be(0);
    const auto t1 = rdtsc_end();
    do_not_optimize(w.pos);
    g_latencies[i] = t1 - t0;
  }
  print_stats("WireWriter (5 fields)", compute_stats(cal, g_latencies));
}

void bench_wire_reader(const TscCalibration &cal) {
  // Pre-serialize a message for reading.
  alignas(64) std::byte buf[kBufSize]{};
  {
    WireWriter w{.buf = std::span{buf}};
    w.write_u64_be(0xDEADBEEFCAFEBABEULL);
    w.write_u32_be(12345);
    w.write_u32_be(100);
    w.write_u16_be(1);
    w.write_u16_be(0);
  }

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    WireReader r{.buf = std::span<const std::byte>{buf, kBufSize}};
    do_not_optimize(r.read_u64_be());
    do_not_optimize(r.read_u32_be());
    do_not_optimize(r.read_u32_be());
    do_not_optimize(r.read_u16_be());
    do_not_optimize(r.read_u16_be());
  }

  for (std::size_t i = 0; i < kN; ++i) {
    WireReader r{.buf = std::span<const std::byte>{buf, kBufSize}};
    const auto t0 = rdtsc_start();
    auto v0 = r.read_u64_be();
    auto v1 = r.read_u32_be();
    auto v2 = r.read_u32_be();
    auto v3 = r.read_u16_be();
    auto v4 = r.read_u16_be();
    const auto t1 = rdtsc_end();
    do_not_optimize(v0);
    do_not_optimize(v1);
    do_not_optimize(v2);
    do_not_optimize(v3);
    do_not_optimize(v4);
    g_latencies[i] = t1 - t0;
  }
  print_stats("WireReader (5 fields)", compute_stats(cal, g_latencies));
}

void bench_wire_roundtrip(const TscCalibration &cal) {
  alignas(64) std::byte buf[kBufSize]{};

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    // Write.
    WireWriter w{.buf = std::span{buf}};
    w.write_u64_be(0xDEADBEEF + i);
    w.write_u32_be(12345);
    w.write_u32_be(100);
    w.write_u16_be(1);
    w.write_u16_be(0);
    // Read back.
    WireReader r{.buf = std::span<const std::byte>{buf, w.written()}};
    auto v0 = r.read_u64_be();
    auto v1 = r.read_u32_be();
    auto v2 = r.read_u32_be();
    auto v3 = r.read_u16_be();
    auto v4 = r.read_u16_be();
    const auto t1 = rdtsc_end();
    do_not_optimize(v0);
    do_not_optimize(v1);
    do_not_optimize(v2);
    do_not_optimize(v3);
    do_not_optimize(v4);
    g_latencies[i] = t1 - t0;
  }
  print_stats("Round-trip (5 fields)", compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== WireCodec Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n", kN);
  std::printf("Message: [u64][u32][u32][u16][u16] = 20 bytes\n\n");

  print_header();
  bench_wire_writer(cal);
  bench_wire_reader(cal);
  bench_wire_roundtrip(cal);

  std::printf("\nTip: taskset -c N ./wire_codec_bench  for stable p99.\n");

  return 0;
}
