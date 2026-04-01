/**
 * @file spread_strategy.hpp
 * @brief Spread-based trading strategy (SpreadStrategy).
 *
 * Tracks BBO per symbol, generates signals when spread > threshold.
 * Alternating buy/sell for position balance. Aggressive pricing (crosses
 * the spread) to ensure fills.
 *
 * Design:
 *   - Zero allocation (inline arrays for per-symbol state).
 *   - Single function call per market data update.
 *   - Returns bool + output ref (hot-path pattern, avoids optional overhead).
 *   - Satisfies StrategyPolicy concept (see strategy_policy.hpp).
 */

#pragma once

#include "strategy_policy.hpp" // Signal, kMaxSymbols, StrategyPolicy

#include <array>
#include <atomic>
#include <cstdint>

namespace mk::app {

/// Spread-based trading strategy, templated on max symbol count.
/// @tparam NSymbols  Compile-time universe size. Sizes the per-symbol BBO
///                   array and is exposed as kMaxSymbols for the pipeline
///                   (StrategyCtx allocation, event loop symbol validation).
template <std::uint32_t NSymbols>
class SpreadStrategy {
public:
  /// Compile-time symbol universe size. Used by the pipeline to size
  /// StrategyCtx buffers and validate incoming symbol IDs.
  static constexpr std::uint32_t kMaxSymbols = NSymbols;

  /// @param spread_threshold Minimum spread (in price ticks) to trigger
  ///        a signal. E.g., 1000 = $0.10 in fixed-point (price * 10000).
  /// @param default_qty Order quantity for generated signals.
  SpreadStrategy(algo::Price spread_threshold, algo::Qty default_qty) noexcept
      : spread_threshold_(spread_threshold), default_qty_(default_qty) {}

  /// Process a market data update and potentially generate a trading signal.
  /// Routes to the correct per-symbol BBO state by symbol_id.
  /// @param update Deserialized market data (symbol_id pre-validated by caller).
  /// @param out    Signal written only when returning true.
  /// @return true if a signal was generated, false otherwise.
  [[nodiscard]] bool on_market_data(const MarketDataUpdate &update,
                                    Signal &out) noexcept {
    // Skip trade messages — only BBO updates affect strategy state.
    if (update.md_msg_type != MdMsgType::kBBOUpdate) {
      return false;
    }

    // Defensive bounds check — each component validates its own inputs.
    if (update.symbol_id == 0 || update.symbol_id > NSymbols) [[unlikely]] {
      return false;
    }
    const auto idx = update.symbol_id - 1;
    auto &bbo = bbo_[idx];

    // Update per-symbol BBO state.
    if (update.side == algo::Side::kBid) {
      bbo.best_bid = update.price;
      bbo.has_bid = true;
    } else {
      bbo.best_ask = update.price;
      bbo.has_ask = true;
    }

    // Need both sides to compute spread.
    if (!bbo.has_bid || !bbo.has_ask) {
      return false;
    }

    // Validate BBO (ask should be > bid in a normal market).
    if (bbo.best_ask <= bbo.best_bid) [[unlikely]] {
      return false;
    }

    const auto spread = bbo.best_ask - bbo.best_bid;

    // Signal: spread wider than threshold → place aggressive (crossing) order.
    // Buy at the ask to lift the offer, sell at the bid to hit the bid.
    // This ensures the order crosses against resting orders in the
    // MatchingEngine, producing fills. Alternate sides for position balance.
    if (spread > spread_threshold_) {
      out.side = bbo.buy_next ? algo::Side::kBid : algo::Side::kAsk;
      // Aggressive pricing: cross the spread to get filled.
      out.price =
          (out.side == algo::Side::kBid) ? bbo.best_ask : bbo.best_bid;
      out.qty = default_qty_;
      out.symbol_id = update.symbol_id;
      bbo.buy_next = !bbo.buy_next;
      signals_generated_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }

    return false;
  }

  // -- Observers --

  [[nodiscard]] algo::Price best_bid(std::uint32_t symbol_id = 1) const noexcept {
    return bbo_[symbol_id - 1].best_bid;
  }
  [[nodiscard]] algo::Price best_ask(std::uint32_t symbol_id = 1) const noexcept {
    return bbo_[symbol_id - 1].best_ask;
  }
  [[nodiscard]] algo::Price spread(std::uint32_t symbol_id = 1) const noexcept {
    const auto &bbo = bbo_[symbol_id - 1];
    if (!bbo.has_bid || !bbo.has_ask) {
      return 0;
    }
    return bbo.best_ask - bbo.best_bid;
  }
  [[nodiscard]] std::uint64_t signals_generated() const noexcept {
    return signals_generated_.load(std::memory_order_relaxed);
  }

private:
  algo::Price spread_threshold_;
  algo::Qty default_qty_;

  /// Per-symbol BBO state.
  struct SymbolBBO {
    algo::Price best_bid{0};
    algo::Price best_ask{0};
    bool has_bid{false};
    bool has_ask{false};
    bool buy_next{true}; // Per-symbol side alternation
  };
  std::array<SymbolBBO, NSymbols> bbo_{};
  // Atomic for cross-thread monitoring safety (relaxed — zero overhead on x86-64).
  std::atomic<std::uint64_t> signals_generated_{0};
};

} // namespace mk::app
