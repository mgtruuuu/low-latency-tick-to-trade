#include <gtest/gtest.h>

#include "string_pack.hpp"

namespace mk::sys {
namespace {

// =============================================================================
// Compile-time constants — verifies constexpr evaluation
// =============================================================================

constexpr auto kBtcUsd = pack_string("BTC-USD");
constexpr auto kEthUsd = pack_string("ETH-USD");
constexpr auto kEmpty = pack_string("");
constexpr auto kMax8 = pack_string("ABCDEFGH"); // exactly 8 chars
constexpr auto kSingle = pack_string("X");

static_assert(kBtcUsd != 0);
static_assert(kEthUsd != 0);
static_assert(kBtcUsd != kEthUsd);
static_assert(kEmpty == 0);
static_assert(kSingle == static_cast<std::uint64_t>('X'));

// =============================================================================
// Basic packing
// =============================================================================

TEST(StringPackTest, EmptyStringReturnsZero) {
  EXPECT_EQ(pack_string(""), 0U);
  EXPECT_EQ(pack_string("", 0), 0U);
}

TEST(StringPackTest, SingleCharPacksToLsb) {
  // 'A' == 0x41, should sit in the lowest byte.
  EXPECT_EQ(pack_string("A"), static_cast<std::uint64_t>('A'));
}

TEST(StringPackTest, KnownSymbolsAreDistinct) {
  EXPECT_NE(pack_string("BTC-USD"), pack_string("ETH-USD"));
  EXPECT_NE(pack_string("AAPL"), pack_string("MSFT"));
}

TEST(StringPackTest, ExactlyEightChars) {
  // Should not abort — 8 is the maximum.
  const auto packed = pack_string("ABCDEFGH");
  EXPECT_NE(packed, 0U);
}

// =============================================================================
// Little-endian byte order verification
// =============================================================================

TEST(StringPackTest, LittleEndianLayout) {
  // "AB" -> 'A' in byte 0 (LSB), 'B' in byte 1.
  const auto packed = pack_string("AB");
  EXPECT_EQ(packed & 0xFF, static_cast<std::uint64_t>('A'));
  EXPECT_EQ((packed >> 8) & 0xFF, static_cast<std::uint64_t>('B'));
}

TEST(StringPackTest, AllBytesPopulated) {
  const auto packed = pack_string("ABCDEFGH");
  for (std::size_t i = 0; i < 8; ++i) {
    const auto byte_val = (packed >> (i * 8)) & 0xFF;
    EXPECT_EQ(byte_val, static_cast<std::uint64_t>('A' + i));
  }
}

// =============================================================================
// Runtime vs compile-time consistency
// =============================================================================

TEST(StringPackTest, RuntimeMatchesCompileTime) {
  // Runtime: ptr+len overload must produce same result as constexpr.
  const char *sym = "BTC-USD";
  EXPECT_EQ(pack_string(sym, 7), kBtcUsd);
  EXPECT_EQ(pack_string(sym, 7), pack_string("BTC-USD"));
}

TEST(StringPackTest, StringViewOverloadMatchesPtrLen) {
  const char data[] = "ETH-USD";
  EXPECT_EQ(pack_string(std::string_view(data, 7)), pack_string(data, 7));
}

// =============================================================================
// Prefix distinctness — shorter strings zero-pad upper bytes
// =============================================================================

TEST(StringPackTest, PrefixesAreDistinct) {
  // "AB" and "ABC" must differ (upper bytes are zero vs 'C').
  EXPECT_NE(pack_string("AB"), pack_string("ABC"));
}

TEST(StringPackTest, SamePrefixDifferentLengthsDistinct) {
  EXPECT_NE(pack_string("A"), pack_string("AA"));
  EXPECT_NE(pack_string("BTC"), pack_string("BTC-"));
}

// =============================================================================
// Switch dispatch — the primary use case
// =============================================================================

TEST(StringPackTest, SwitchDispatch) {
  constexpr auto kAapl = pack_string("AAPL");
  constexpr auto kMsft = pack_string("MSFT");

  auto classify = [](std::uint64_t id) -> int {
    switch (id) {
    case kAapl:
      return 1;
    case kMsft:
      return 2;
    default:
      return 0;
    }
  };

  EXPECT_EQ(classify(pack_string("AAPL")), 1);
  EXPECT_EQ(classify(pack_string("MSFT")), 2);
  EXPECT_EQ(classify(pack_string("GOOG")), 0);
}

// =============================================================================
// Death tests — abort on > 8 chars
// =============================================================================

using StringPackDeathTest = ::testing::Test;

TEST_F(StringPackDeathTest, AbortOnTooLongStringView) {
  EXPECT_DEATH((void)pack_string("123456789"), "");
}

TEST_F(StringPackDeathTest, AbortOnTooLongPtrLen) {
  const char *long_sym = "SAMSUNG-ELEC";
  EXPECT_DEATH((void)pack_string(long_sym, 12), "");
}

} // namespace
} // namespace mk::sys
