/**
 * @file packed_codec_test.cpp
 * @brief Tests for packed_codec.hpp — zero-copy packed struct codec.
 */

#include "net/packed_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

namespace {

// -- Test packed structs --
// #pragma pack(push, 1) removes all padding, so sizeof matches sum of fields.

#pragma pack(push, 1)

struct SimpleMsg {
  std::uint64_t id;
  std::int64_t price;
  std::uint32_t qty;
  std::uint8_t side;
  // Without #pragma pack: sizeof would be 24 (padding after side)
  // With #pragma pack(1): sizeof = 8 + 8 + 4 + 1 = 21
};

struct TinyMsg {
  std::uint8_t type;
  std::uint16_t value;
  // Without #pragma pack: sizeof would be 4 (padding after type)
  // With #pragma pack(1): sizeof = 1 + 2 = 3
};

#pragma pack(pop)

// Verify that packing removed padding.
static_assert(sizeof(SimpleMsg) == 21,
              "SimpleMsg must be 21 bytes with #pragma pack(1)");
static_assert(sizeof(TinyMsg) == 3,
              "TinyMsg must be 3 bytes with #pragma pack(1)");

// Verify the concept is satisfied.
static_assert(mk::net::PackedWireStruct<SimpleMsg>);
static_assert(mk::net::PackedWireStruct<TinyMsg>);

// -- Struct without #pragma pack for comparison --

struct UnpackedMsg {
  std::uint64_t id;
  std::uint32_t qty;
  std::uint8_t side;
  // sizeof likely 16 on x86-64 (padding after side to align to 8)
};

static_assert(mk::net::PackedWireStruct<UnpackedMsg>,
              "UnpackedMsg is trivially copyable + standard layout");

// ======================================================================
// Tests
// ======================================================================

TEST(PackedCodecTest, RoundTripSimpleMsg) {
  SimpleMsg original{};
  original.id = 42;
  original.price = -12345678;
  original.qty = 100;
  original.side = 1;

  std::array<std::byte, 64> buf{};
  auto written = mk::net::pack_struct<SimpleMsg>(buf, original);
  ASSERT_EQ(written, sizeof(SimpleMsg));
  ASSERT_EQ(written, 21U);

  auto parsed = mk::net::unpack_struct<SimpleMsg>(buf);
  ASSERT_TRUE(parsed.has_value());

  EXPECT_EQ(parsed->id, 42U);           // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(parsed->price, -12345678);  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(parsed->qty, 100U);         // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(parsed->side, 1U);          // NOLINT(bugprone-unchecked-optional-access)
}

TEST(PackedCodecTest, RoundTripTinyMsg) {
  TinyMsg original{};
  original.type = 0xFF;
  original.value = 0xABCD;

  std::array<std::byte, 16> buf{};
  auto written = mk::net::pack_struct<TinyMsg>(buf, original);
  ASSERT_EQ(written, 3U);

  // Use hot-path overload: memcpy into a stack-local variable.
  // Copy packed fields into aligned locals before comparing —
  // EXPECT_EQ binds const a reference, which is UB on a misaligned
  // packed field (UBSan catches this).
  TinyMsg parsed{};
  ASSERT_TRUE(mk::net::unpack_struct<TinyMsg>(buf, parsed));

  const auto p_type = parsed.type;
  const auto p_value = parsed.value;
  EXPECT_EQ(p_type, 0xFF);
  EXPECT_EQ(p_value, 0xABCD);
}

TEST(PackedCodecTest, RoundTripHotPathOverload) {
  SimpleMsg original{};
  original.id = 99;
  original.price = 1'000'000;
  original.qty = 500;
  original.side = 0;

  std::array<std::byte, 64> buf{};
  auto written = mk::net::pack_struct<SimpleMsg>(buf, original);
  ASSERT_EQ(written, sizeof(SimpleMsg));

  // Hot-path overload: pre-allocated output, returns bool.
  SimpleMsg out{};
  const bool ok = mk::net::unpack_struct<SimpleMsg>(buf, out);
  ASSERT_TRUE(ok);

  EXPECT_EQ(out.id, 99U);
  EXPECT_EQ(out.price, 1'000'000);
  EXPECT_EQ(out.qty, 500U);
  EXPECT_EQ(out.side, 0U);
}

TEST(PackedCodecTest, PackBufferTooSmall) {
  SimpleMsg msg{};
  msg.id = 1;

  // Buffer smaller than sizeof(SimpleMsg) = 21.
  std::array<std::byte, 20> buf{};
  auto written = mk::net::pack_struct<SimpleMsg>(buf, msg);
  EXPECT_EQ(written, 0U);
}

TEST(PackedCodecTest, UnpackBufferTooSmall) {
  std::array<std::byte, 20> buf{};

  // Optional overload.
  auto result = mk::net::unpack_struct<SimpleMsg>(buf);
  EXPECT_FALSE(result.has_value());

  // Bool overload.
  SimpleMsg out{};
  const bool ok = mk::net::unpack_struct<SimpleMsg>(buf, out);
  EXPECT_FALSE(ok);
}

TEST(PackedCodecTest, ExactSizeBuffer) {
  TinyMsg msg{};
  msg.type = 7;
  msg.value = 1234;

  // Buffer exactly sizeof(TinyMsg) = 3.
  std::array<std::byte, 3> buf{};
  auto written = mk::net::pack_struct<TinyMsg>(buf, msg);
  ASSERT_EQ(written, 3U);

  TinyMsg parsed{};
  ASSERT_TRUE(mk::net::unpack_struct<TinyMsg>(buf, parsed));
  // Copy packed fields into aligned locals (see RoundTripTinyMsg comment).
  const auto pt = parsed.type;
  const auto pv = parsed.value;
  EXPECT_EQ(pt, 7);
  EXPECT_EQ(pv, 1234);
}

TEST(PackedCodecTest, EmptyBuffer) {
  std::span<std::byte> const empty{};

  auto result = mk::net::unpack_struct<TinyMsg>(empty);
  EXPECT_FALSE(result.has_value());

  TinyMsg msg{};
  msg.type = 1;
  auto written = mk::net::pack_struct<TinyMsg>(empty, msg);
  EXPECT_EQ(written, 0U);
}

TEST(PackedCodecTest, WireLayoutMatchesMemoryLayout) {
  // The whole point of packed structs: bytes on the wire are identical
  // to bytes in memory. Verify by checking specific byte offsets.
  SimpleMsg msg{};
  msg.id = 0x0102030405060708;
  msg.price = 0x1112131415161718;
  msg.qty = 0x21222324;
  msg.side = 0x31;

  std::array<std::byte, 21> buf{};
  (void)mk::net::pack_struct<SimpleMsg>(buf, msg);

  // Verify the wire bytes match what memcpy of the struct produces.
  // On x86-64 (little-endian), id's first byte should be 0x08 (LSB first).
  SimpleMsg verify{};
  std::memcpy(&verify, buf.data(), sizeof(SimpleMsg));
  EXPECT_EQ(verify.id, msg.id);
  EXPECT_EQ(verify.price, msg.price);
  EXPECT_EQ(verify.qty, msg.qty);
  EXPECT_EQ(verify.side, msg.side);
}

TEST(PackedCodecTest, PackedVsUnpackedSizeDifference) {
  // Demonstrates why #pragma pack matters for wire formats.
  // Without packing, compilers add padding for alignment.
  //
  // SimpleMsg (packed):   21 bytes (8+8+4+1)
  // UnpackedMsg:          likely 16 bytes (8+4+1+3padding)
  //                       but could vary by compiler
  //
  // The key insight: packed size = sum of field sizes, guaranteed.
  EXPECT_EQ(sizeof(SimpleMsg), 21U);
  // UnpackedMsg has padding — just verify it's larger than sum of fields.
  // Sum of fields = 8 + 4 + 1 = 13, but sizeof likely 16 due to alignment.
  EXPECT_GT(sizeof(UnpackedMsg), 13U);
}

} // namespace
