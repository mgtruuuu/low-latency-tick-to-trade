/**
 * @file packed_codec_bench.cpp
 * @brief Microbenchmark for packed_codec zero-copy serialization.
 *
 * Measures the cost of pack_struct() / unpack_struct() — whole-struct
 * memcpy with single bounds check, no endian conversion.
 *
 * Uses a 21-byte packed order entry struct to simulate realistic HFT
 * internal communication (co-located x86-64 ↔ x86-64).
 *
 * Operations benchmarked:
 *   - pack_struct()                — serialize (memcpy to buffer)
 *   - unpack_struct<T>() optional  — deserialize (returns std::optional)
 *   - unpack_struct(buf, out)      — hot-path deserialize (no optional)
 *   - round-trip                   — pack + unpack
 *
 * Usage:
 *   cmake --build build -j$(nproc)
 *   ./build/bench/net/packed_codec_bench
 *
 * For stable p99, run on an isolated core:
 *   taskset -c 2 ./build/bench/net/packed_codec_bench
 */

#include "bench_utils.hpp"
#include "net/packed_codec.hpp"
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
// Test struct
// ============================================================================

#pragma pack(push, 1)
struct OrderEntry {
  std::uint64_t order_id;
  std::int64_t price;
  std::uint32_t qty;
  std::uint8_t side;
};
#pragma pack(pop)
static_assert(sizeof(OrderEntry) == 21);

// ============================================================================
// Configuration
// ============================================================================

constexpr std::size_t kN = 10'000;
constexpr std::size_t kBufSize = 64;

std::array<std::uint64_t, kN> g_latencies{};

// ============================================================================
// Benchmarks
// ============================================================================

void bench_pack_struct(const TscCalibration &cal) {
  alignas(64) std::byte buf[kBufSize]{};
  OrderEntry order{.order_id = 0xDEADBEEFCAFEBABEULL, .price = 12345, .qty = 100, .side = 1};

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    do_not_optimize(pack_struct(std::span{buf}, order));
    ++order.order_id;
  }

  for (std::size_t i = 0; i < kN; ++i) {
    order.order_id = i;
    const auto t0 = rdtsc_start();
    auto n = pack_struct(std::span{buf}, order);
    const auto t1 = rdtsc_end();
    do_not_optimize(n);
    g_latencies[i] = t1 - t0;
  }
  print_stats("pack_struct()", compute_stats(cal, g_latencies));
}

void bench_unpack_struct_optional(const TscCalibration &cal) {
  alignas(64) std::byte buf[kBufSize]{};
  const OrderEntry order{.order_id = 0xDEADBEEFCAFEBABEULL, .price = 12345, .qty = 100, .side = 1};
  (void)pack_struct(std::span{buf}, order);

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    auto parsed = unpack_struct<OrderEntry>(std::span<const std::byte>{buf, kBufSize});
    do_not_optimize(parsed);
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    auto parsed = unpack_struct<OrderEntry>(std::span<const std::byte>{buf, kBufSize});
    const auto t1 = rdtsc_end();
    do_not_optimize(parsed);
    g_latencies[i] = t1 - t0;
  }
  print_stats("unpack<T>() optional", compute_stats(cal, g_latencies));
}

void bench_unpack_struct_hotpath(const TscCalibration &cal) {
  alignas(64) std::byte buf[kBufSize]{};
  const OrderEntry order{.order_id = 0xDEADBEEFCAFEBABEULL, .price = 12345, .qty = 100, .side = 1};
  (void)pack_struct(std::span{buf}, order);
  OrderEntry out{};

  // Warm-up.
  for (int i = 0; i < 500; ++i) {
    do_not_optimize(unpack_struct(std::span<const std::byte>{buf, kBufSize}, out));
  }

  for (std::size_t i = 0; i < kN; ++i) {
    const auto t0 = rdtsc_start();
    auto ok = unpack_struct(std::span<const std::byte>{buf, kBufSize}, out);
    const auto t1 = rdtsc_end();
    do_not_optimize(ok);
    do_not_optimize(out);
    g_latencies[i] = t1 - t0;
  }
  print_stats("unpack(buf,out) hot", compute_stats(cal, g_latencies));
}

void bench_roundtrip(const TscCalibration &cal) {
  alignas(64) std::byte buf[kBufSize]{};

  for (std::size_t i = 0; i < kN; ++i) {
    const OrderEntry order{.order_id = i, .price = 12345, .qty = 100, .side = 1};
    OrderEntry out{};
    const auto t0 = rdtsc_start();
    (void)pack_struct(std::span{buf}, order);
    (void)unpack_struct(std::span<const std::byte>{buf, kBufSize}, out);
    const auto t1 = rdtsc_end();
    do_not_optimize(out);
    g_latencies[i] = t1 - t0;
  }
  print_stats("round-trip", compute_stats(cal, g_latencies));
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("=== PackedCodec Microbenchmark ===\n\n");

  auto cal = calibrate_and_print();
  std::printf("Iterations per benchmark: %zu\n", kN);
  std::printf("Struct size: %zu bytes (packed)\n\n", sizeof(OrderEntry));

  print_header();
  bench_pack_struct(cal);
  bench_unpack_struct_optional(cal);
  bench_unpack_struct_hotpath(cal);
  bench_roundtrip(cal);

  std::printf("\nTip: taskset -c N ./packed_codec_bench  for stable p99.\n");

  return 0;
}
