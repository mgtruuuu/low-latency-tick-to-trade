/**
 * @file trading_types.hpp
 * @brief Fundamental trading type aliases shared across the algo library.
 *
 * Vocabulary types used by OrderBook, MatchingEngine, and application-level
 * protocols. Separated from order_book.hpp so that lightweight consumers
 * (e.g., wire protocol definitions) can include just the types without
 * pulling in the full order book implementation.
 */

#pragma once

#include <cstdint>

namespace mk::algo {

/// Fixed-point tick price. Exchanges use integer ticks (e.g., price * 10000)
/// to avoid floating-point comparison issues on the hot path.
using Price = std::int64_t;

/// Unique order identifier. Assigned by the exchange or OMS.
using OrderId = std::uint64_t;

/// Order quantity in lots/shares. uint32_t supports up to ~4 billion lots.
using Qty = std::uint32_t;

/// Order side. uint8_t underlying for compact storage. Values 0/1 allow
/// direct array indexing via side_index(): level_map_[side_index(side)].
enum class Side : std::uint8_t { kBid = 0, kAsk, kCount };

} // namespace mk::algo
