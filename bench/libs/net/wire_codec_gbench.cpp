/**
 * @file wire_codec_gbench.cpp
 * @brief Google Benchmark version of WireWriter/WireReader benchmarks.
 *
 * Companion to wire_codec_bench.cpp (custom rdtsc). This version uses
 * Google Benchmark for CI-friendly output, regression tracking, and JSON
 * export.
 *
 * Simulates a typical order-entry message:
 *   [u64 order_id][u32 price][u32 qty][u16 symbol][u16 side] = 20 bytes
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/net/wire_codec_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/net/wire_codec_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/net/wire_codec_gbench
 */

#include "net/wire_reader.hpp"
#include "net/wire_writer.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

using namespace mk::net;

namespace {

constexpr std::size_t kBufSize = 64;

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_WireWriter(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize]{};
  std::uint64_t order_id = 0xDEADBEEF;
  for (auto _ : state) {
    WireWriter w{.buf = std::span{buf}};
    w.write_u64_be(order_id++);
    w.write_u32_be(12345);
    w.write_u32_be(100);
    w.write_u16_be(1);
    w.write_u16_be(0);
    benchmark::DoNotOptimize(w.pos);
  }
}
BENCHMARK(BM_WireWriter);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_WireReader(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize]{};
  // Pre-serialize.
  WireWriter w{.buf = std::span{buf}};
  w.write_u64_be(0xDEADBEEFCAFEBABEULL);
  w.write_u32_be(12345);
  w.write_u32_be(100);
  w.write_u16_be(1);
  w.write_u16_be(0);

  for (auto _ : state) {
    WireReader r{.buf = std::span<const std::byte>{buf, kBufSize}};
    auto v0 = r.read_u64_be();
    auto v1 = r.read_u32_be();
    auto v2 = r.read_u32_be();
    auto v3 = r.read_u16_be();
    auto v4 = r.read_u16_be();
    benchmark::DoNotOptimize(v0);
    benchmark::DoNotOptimize(v1);
    benchmark::DoNotOptimize(v2);
    benchmark::DoNotOptimize(v3);
    benchmark::DoNotOptimize(v4);
  }
}
BENCHMARK(BM_WireReader);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_WireRoundtrip(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize]{};
  std::uint64_t order_id = 0xDEADBEEF;
  for (auto _ : state) {
    WireWriter w{.buf = std::span{buf}};
    w.write_u64_be(order_id++);
    w.write_u32_be(12345);
    w.write_u32_be(100);
    w.write_u16_be(1);
    w.write_u16_be(0);

    WireReader r{.buf = std::span<const std::byte>{buf, w.written()}};
    auto v0 = r.read_u64_be();
    auto v1 = r.read_u32_be();
    auto v2 = r.read_u32_be();
    auto v3 = r.read_u16_be();
    auto v4 = r.read_u16_be();
    benchmark::DoNotOptimize(v0);
    benchmark::DoNotOptimize(v1);
    benchmark::DoNotOptimize(v2);
    benchmark::DoNotOptimize(v3);
    benchmark::DoNotOptimize(v4);
  }
}
BENCHMARK(BM_WireRoundtrip);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
