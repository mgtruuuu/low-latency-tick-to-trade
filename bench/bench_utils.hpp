/**
 * @file bench_utils.hpp
 * @brief Shared benchmark infrastructure for custom rdtsc benchmarks.
 *
 * Extracted from bench/algo/order_book_bench.cpp to avoid duplication.
 * Uses TscCalibration from nano_clock.hpp for multi-sample calibration.
 *
 * Provides:
 *   - do_not_optimize<T>() — compiler escape (prevents dead code elimination)
 *   - Stats / compute_stats() — percentile computation from raw cycle counts
 *   - print_header() / print_stats() — formatted table output
 *   - calibrate_and_print() — main() boilerplate for TSC setup
 */

#pragma once

#include "sys/nano_clock.hpp"

#include <cstdint>
#include <span>

namespace mk::bench {

// ============================================================================
// Compiler escape
// ============================================================================

/// Prevent the compiler from optimizing away a computed value.
/// Inline asm tells the compiler the value is "used" without executing
/// any actual instructions at runtime (zero overhead).
template <typename T> inline void do_not_optimize(const T &val) {
  asm volatile("" : : "r,m"(val) : "memory");
}

// ============================================================================
// Statistics
// ============================================================================

struct Stats {
  double min_ns;
  double median_ns;
  double p99_ns;
  double max_ns;
};

/// Sort raw cycle counts and compute percentile statistics.
/// Automatically subtracts rdtsc_start()/rdtsc_end() overhead (measured
/// during calibration) from each sample before conversion to nanoseconds.
/// @param cal   TSC calibration (includes overhead_cycles).
/// @param data  Raw TSC cycle counts (will be modified and sorted in-place).
Stats compute_stats(const sys::TscCalibration &cal,
                    std::span<std::uint64_t> data);

// ============================================================================
// Output formatting
// ============================================================================

void print_header();

void print_stats(const char *name, const Stats &s);

// ============================================================================
// Main() boilerplate
// ============================================================================

/// Calibrate TSC and print result. Call once at the start of main().
/// Returns the calibration object for use with compute_stats().
[[nodiscard]] sys::TscCalibration calibrate_and_print();

} // namespace mk::bench
