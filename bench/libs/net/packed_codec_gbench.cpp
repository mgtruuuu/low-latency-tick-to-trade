/**
 * @file packed_codec_gbench.cpp
 * @brief Google Benchmark version of packed_codec benchmarks.
 *
 * Companion to packed_codec_bench.cpp (custom rdtsc). This version uses
 * Google Benchmark for CI-friendly output, regression tracking, and JSON
 * export.
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/net/packed_codec_gbench
 *
 * JSON output for regression tracking:
 *   ./build/bench/net/packed_codec_gbench --benchmark_format=json
 *
 * For stable results, run on an isolated core:
 *   taskset -c 2 ./build/bench/net/packed_codec_gbench
 */

#include "net/packed_codec.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

using namespace mk::net;

namespace {

#pragma pack(push, 1)
struct OrderEntry {
  std::uint64_t order_id;
  std::int64_t price;
  std::uint32_t qty;
  std::uint8_t side;
};
#pragma pack(pop)
static_assert(sizeof(OrderEntry) == 21);

constexpr std::size_t kBufSize = 64;

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_PackStruct(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize]{};
  OrderEntry order{.order_id = 0xDEADBEEFCAFEBABEULL, .price = 12345, .qty = 100, .side = 1};
  for (auto _ : state) {
    auto n = pack_struct(std::span{buf}, order);
    benchmark::DoNotOptimize(n);
    ++order.order_id;
  }
}
BENCHMARK(BM_PackStruct);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_UnpackStructOptional(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize]{};
  const OrderEntry order{.order_id = 0xDEADBEEFCAFEBABEULL, .price = 12345, .qty = 100, .side = 1};
  (void)pack_struct(std::span{buf}, order);

  for (auto _ : state) {
    auto parsed =
        unpack_struct<OrderEntry>(std::span<const std::byte>{buf, kBufSize});
    benchmark::DoNotOptimize(parsed);
  }
}
BENCHMARK(BM_UnpackStructOptional);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_UnpackStructHotPath(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize]{};
  const OrderEntry order{.order_id = 0xDEADBEEFCAFEBABEULL, .price = 12345, .qty = 100, .side = 1};
  (void)pack_struct(std::span{buf}, order);
  OrderEntry out{};

  for (auto _ : state) {
    auto ok =
        unpack_struct(std::span<const std::byte>{buf, kBufSize}, out);
    benchmark::DoNotOptimize(ok);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_UnpackStructHotPath);

// NOLINTNEXTLINE(readability-identifier-naming)
void BM_PackedRoundtrip(benchmark::State &state) {
  alignas(64) std::byte buf[kBufSize]{};
  OrderEntry out{};
  std::uint64_t id = 0;

  for (auto _ : state) {
    const OrderEntry order{.order_id = id++, .price = 12345, .qty = 100, .side = 1};
    (void)pack_struct(std::span{buf}, order);
    (void)unpack_struct(std::span<const std::byte>{buf, kBufSize}, out);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_PackedRoundtrip);

} // namespace

// BENCHMARK_MAIN() is provided by benchmark::benchmark_main link target.
