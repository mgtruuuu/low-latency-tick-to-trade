/**
 * @file nano_clock.hpp
 * @brief Nanosecond-resolution clock utilities for HFT latency measurement.
 *
 * Target: Linux x86-64 only.
 *
 * Three timing sources, each serving a different purpose:
 *
 *   1. monotonic_nanos()
 *      - Uses CLOCK_MONOTONIC via clock_gettime().
 *      - Returns nanoseconds since an arbitrary epoch (typically boot time).
 *      - Unaffected by NTP step adjustments — never jumps backward.
 *      - NTP can still slew (gradually adjust) the rate.
 *      - Use for: latency measurement, elapsed time, timeouts.
 *      - Cost: ~20ns on modern Linux (vDSO, no real syscall).
 *
 *   1b. monotonic_raw_nanos()
 *      - Uses CLOCK_MONOTONIC_RAW via clock_gettime().
 *      - Like CLOCK_MONOTONIC but fully immune to NTP (no slewing either).
 *      - May drift from wall clock over long periods (hours+).
 *      - Use for: benchmarks where NTP rate adjustment could bias results.
 *      - Cost: ~20ns (same vDSO mechanism).
 *
 *   2. realtime_nanos()
 *      - Uses CLOCK_REALTIME via clock_gettime().
 *      - Returns nanoseconds since Unix epoch (1970-01-01 00:00:00 UTC).
 *      - CAN jump forward or backward due to NTP corrections.
 *      - Use for: wall-clock timestamps, logging, exchange protocol fields.
 *      - Cost: ~20ns (same vDSO mechanism).
 *
 *   3. rdtsc()
 *      - Reads the CPU Time Stamp Counter directly via the RDTSC instruction.
 *      - Returns raw CPU cycles (NOT nanoseconds). Must be calibrated to
 *        convert to wall-clock time.
 *      - Lowest possible overhead (~1ns).
 *      - Use for: ultra-low-latency hot path measurement where even
 *        clock_gettime() overhead matters.
 *      - Requires invariant TSC (constant_tsc + nonstop_tsc CPU flags).
 *        Call assert_tsc_reliable() at startup to verify.
 *        Verify manually: grep -E "constant_tsc|nonstop_tsc" /proc/cpuinfo
 *
 * TSC calibration:
 *   TscCalibration::calibrate() measures TSC frequency at startup by
 *   busy-waiting against clock_gettime(CLOCK_MONOTONIC). Multi-sample
 *   with maximum selection to filter OS preemption at read boundaries.
 *   Use to_ns() to convert raw cycle counts to nanoseconds. Cold-path only.
 *
 * Why not std::chrono?
 *   std::chrono::steady_clock and high_resolution_clock ultimately call
 *   clock_gettime() on Linux. At -O2, the compiler optimizes away the
 *   duration_cast and type conversion layers — runtime cost is identical.
 *   We use clock_gettime() directly for code clarity: the return type is
 *   a plain std::int64_t with no template machinery to reason about
 *   when debugging hot paths.
 */

#pragma once

#include <cpuid.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <x86intrin.h>

#include "log/signal_logger.hpp"

namespace mk::sys {

/**
 * @brief Returns nanoseconds from a monotonic clock (CLOCK_MONOTONIC).
 *
 * Monotonic means the value only increases — it never jumps backward,
 * even if the system clock is adjusted by NTP. This makes it safe for
 * measuring elapsed time between two points.
 *
 * Usage:
 *   auto start = mk::sys::monotonic_nanos();
 *   // ... hot path code ...
 *   auto end = mk::sys::monotonic_nanos();
 *   auto latency_ns = end - start;
 */
[[nodiscard]] inline std::int64_t monotonic_nanos() noexcept {
  struct timespec ts {};
  // clock_gettime with CLOCK_MONOTONIC:
  //   - Implemented via vDSO on Linux (no kernel entry).
  //   - Reads a shared memory page maintained by the kernel.
  //   - Typically ~20ns per call.
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000) + ts.tv_nsec;
}

/**
 * @brief Returns nanoseconds from the real-time clock (CLOCK_REALTIME).
 *
 * Returns nanoseconds since Unix epoch (1970-01-01 00:00:00 UTC).
 * WARNING: This clock CAN jump backward due to NTP adjustments.
 * Do NOT use for elapsed time measurement — use monotonic_nanos() instead.
 *
 * Usage:
 *   auto timestamp = mk::sys::realtime_nanos();  // for logging / exchange msgs
 */
[[nodiscard]] inline std::int64_t realtime_nanos() noexcept {
  struct timespec ts {};
  clock_gettime(CLOCK_REALTIME, &ts);
  return (static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000) + ts.tv_nsec;
}

/**
 * @brief Returns nanoseconds from CLOCK_MONOTONIC_RAW.
 *
 * Like monotonic_nanos() but completely immune to NTP adjustments —
 * neither step corrections NOR slew adjustments affect this clock.
 * The trade-off: it may drift from wall-clock time over long periods
 * (hours+) because the hardware oscillator is not NTP-disciplined.
 *
 * Use for: benchmarks and profiling where NTP rate slewing could
 * bias sub-microsecond measurements.
 */
[[nodiscard]] inline std::int64_t monotonic_raw_nanos() noexcept {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000) + ts.tv_nsec;
}

/**
 * @brief Reads the CPU Time Stamp Counter via the RDTSC instruction.
 *
 * Returns raw CPU cycle count. This is NOT nanoseconds — to convert,
 * you need the CPU's TSC frequency:
 *   nanoseconds = (cycles * 1'000'000'000) / tsc_frequency_hz
 *
 * The __rdtsc() intrinsic compiles to the RDTSC instruction, which has
 * ~1ns overhead — the lowest of any timing source.
 *
 * Requires invariant TSC — call assert_tsc_reliable() once at startup.
 *
 * WARNING: RDTSC is NOT a serializing instruction. On out-of-order CPUs,
 * the processor can execute RDTSC before prior instructions complete or
 * after subsequent instructions start, causing the measurement interval
 * to not precisely bracket the code being timed. For accurate benchmarks,
 * use rdtsc_start() / rdtsc_end() instead.
 */
[[nodiscard]] inline std::uint64_t rdtsc() noexcept { return __rdtsc(); }

/**
 * @brief Serialized TSC read for the START of a measurement interval.
 *
 * Executes LFENCE before RDTSC. LFENCE is a dispatch serializing
 * instruction on all modern x86 CPUs (Intel since Pentium Pro, AMD since
 * Spectre mitigation) — it drains the instruction pipeline, ensuring
 * all prior instructions have completed before the timestamp is taken.
 *
 * Pair with rdtsc_end() for bracketed measurement:
 *   auto t0 = rdtsc_start();
 *   // ... code under test ...
 *   auto t1 = rdtsc_end();
 *   auto cycles = t1 - t0;
 */
[[nodiscard]] inline std::uint64_t rdtsc_start() noexcept {
  // Two levels of instruction ordering matter here:
  //   1. CPU reordering: out-of-order execution can run RDTSC before prior
  //      instructions complete. LFENCE drains the reorder buffer (ROB).
  //   2. Compiler reordering: the optimizer can rearrange instructions at
  //      compile time. The "memory" clobber prevents this.
  //
  // asm volatile("lfence" ::: "memory") provides BOTH in a single statement.
  // This is the form used by the Linux kernel and DPDK.
  //
  // Note: _mm_lfence() also works on GCC/Clang (the builtin is memory-
  // clobbering), but that is a compiler implementation detail — the asm
  // volatile form is guaranteed by GCC's inline asm specification.
  asm volatile("lfence" ::: "memory");
  return __rdtsc();
}

/**
 * @brief Serialized TSC read for the END of a measurement interval.
 *
 * Uses RDTSCP, which waits for all prior instructions to complete
 * before reading the timestamp (it is "pseudo-serializing" — serializes
 * instructions before it but not after). A trailing LFENCE prevents
 * subsequent instructions from being speculatively executed before the
 * timestamp is read, which matters in measurement loops where the next
 * iteration's setup could otherwise overlap.
 *
 * Why RDTSCP instead of LFENCE+RDTSC?
 *   RDTSCP is a single instruction that both serializes and reads TSC,
 *   giving slightly lower overhead (~1-2 cycles) than the two-instruction
 *   LFENCE+RDTSC sequence. It also reads the IA32_TSC_AUX MSR (which
 *   stores the processor ID), though we discard that value here.
 */
[[nodiscard]] inline std::uint64_t rdtsc_end() noexcept {
  unsigned int aux = 0;
  const std::uint64_t tsc = __rdtscp(&aux);
  // Trailing LFENCE + compiler barrier: prevent instructions AFTER this
  // point from being pulled before the RDTSCP (CPU) or reordered by the
  // optimizer (compiler). See rdtsc_start() for rationale.
  asm volatile("lfence" ::: "memory");
  return tsc;
}

// ============================================================================
// TSC calibration
// ============================================================================

/**
 * @brief TSC frequency calibration for converting rdtsc() cycles to
 * nanoseconds.
 *
 * Measures how many TSC cycles pass during a known wall-clock interval
 * (default 100ms via CLOCK_MONOTONIC). This is the standard approach —
 * even Google Benchmark does this internally.
 *
 * Cold-path only — call calibrate() once at startup, then use to_ns()
 * to convert cycle counts collected on the hot path.
 *
 * Usage:
 *   auto cal = mk::sys::TscCalibration::calibrate();
 *   printf("TSC: %.3f GHz\n", cal.freq_ghz());
 *
 *   auto t0 = rdtsc_start();
 *   // ... code under test ...
 *   auto t1 = rdtsc_end();
 *   printf("latency: %.1f ns\n", cal.to_ns(t1 - t0));
 */
struct TscCalibration {
  std::uint64_t freq_hz;
  std::uint64_t overhead_cycles;

  /// No default construction — must use calibrate() or provide a known
  /// frequency.
  TscCalibration() = delete;

  /// Construct with a known TSC frequency in Hz and overhead in cycles.
  explicit constexpr TscCalibration(std::uint64_t hz,
                                    std::uint64_t overhead = 0) noexcept
      : freq_hz(hz), overhead_cycles(overhead) {}

  /// Measure the overhead of a serialized rdtsc_start()/rdtsc_end() pair.
  /// Returns the minimum observed cost in cycles (typically 30-40 cycles).
  /// Minimum is the correct statistic: noise only adds to the measurement,
  /// so the minimum is closest to the true intrinsic cost.
  [[nodiscard]] static std::uint64_t measure_overhead() noexcept {
    auto best = std::uint64_t(-1);
    for (int i = 0; i < 10'000; ++i) {
      const auto t0 = rdtsc_start();
      const auto t1 = rdtsc_end();
      const auto elapsed = t1 - t0;
      best = std::min(best, elapsed);
    }
    return best;
  }

  /// Busy-wait calibration. Default: 5 x 20ms = 100ms total.
  /// Also measures rdtsc_start()/rdtsc_end() overhead and stores it
  /// for automatic subtraction in benchmark statistics.
  ///
  /// Multiple samples defend against a boundary race condition:
  /// if the OS preempts between the TSC and wall-clock reads at the
  /// end of a sample, the wall-clock elapsed time is inflated while
  /// the TSC elapsed time is correct — yielding a frequency that is
  /// too low. Taking the maximum across samples filters this out,
  /// because preemption can only deflate freq_hz, never inflate it.
  [[nodiscard]] static TscCalibration
  calibrate(std::int64_t duration_ns = 100'000'000, int samples = 5) noexcept {
    const auto per_sample_ns = duration_ns / samples;
    std::uint64_t best_freq = 0;

    for (int s = 0; s < samples; ++s) {
      const auto wall_start = monotonic_nanos();
      const auto tsc_start = rdtsc_start();

      while (monotonic_nanos() - wall_start < per_sample_ns) {
        // busy-wait for the calibration interval
      }

      const auto tsc_end = rdtsc_end();
      const auto wall_end = monotonic_nanos();

      const auto elapsed_ns = static_cast<std::uint64_t>(wall_end - wall_start);
      const auto elapsed_tsc = tsc_end - tsc_start;
      const std::uint64_t freq = (elapsed_tsc * 1'000'000'000ULL) / elapsed_ns;

      best_freq = std::max(best_freq, freq);
    }

    return TscCalibration{best_freq, measure_overhead()};
  }

  /// Convert raw TSC cycles to nanoseconds.
  [[nodiscard]] double to_ns(std::uint64_t cycles) const noexcept {
    return static_cast<double>(cycles) * 1e9 / static_cast<double>(freq_hz);
  }

  /// TSC frequency in GHz (for display).
  [[nodiscard]] double freq_ghz() const noexcept {
    return static_cast<double>(freq_hz) / 1e9;
  }
};

// ============================================================================
// TSC reliability checks
// ============================================================================

/**
 * @brief Checks if the CPU supports invariant TSC (constant_tsc + nonstop_tsc).
 *
 * Invariant TSC (CPUID leaf 0x80000007, EDX bit 8) guarantees:
 *   - constant_tsc: TSC frequency does not change with CPU power states
 *     (turbo boost, power saving).
 *   - nonstop_tsc: TSC keeps incrementing even during deep sleep (C-states).
 *
 * Without invariant TSC, rdtsc() returns unreliable values — the same
 * wall-clock interval can produce different cycle counts depending on
 * CPU frequency scaling and sleep state.
 *
 * @return true if invariant TSC is supported, false otherwise.
 */
[[nodiscard]] inline bool is_tsc_reliable() noexcept {
  unsigned int eax = 0;
  unsigned int ebx = 0;
  unsigned int ecx = 0;
  unsigned int edx = 0;

  // First check if extended CPUID leaf 0x80000007 is supported.
  // __get_cpuid(0x80000000) returns the maximum supported extended leaf.
  if (__get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx) == 0) {
    return false;
  }
  if (eax < 0x80000007) {
    return false;
  }

  // CPUID leaf 0x80000007 — Advanced Power Management.
  // EDX bit 8 = Invariant TSC (constant_tsc + nonstop_tsc).
  if (__get_cpuid(0x80000007, &eax, &ebx, &ecx, &edx) == 0) {
    return false;
  }
  return (edx & (1U << 8)) != 0;
}

/**
 * @brief Aborts if the CPU does not support invariant TSC.
 *
 * Call once at startup (e.g., in main() or benchmark setup) before using
 * rdtsc() for timing. This is a cold-path validation — never call on
 * the hot path.
 *
 * Usage:
 *   int main() {
 *     mk::sys::assert_tsc_reliable();
 *     // ... safe to use rdtsc() ...
 *   }
 */
inline void assert_tsc_reliable() noexcept {
  if (!is_tsc_reliable()) [[unlikely]] {
    log::signal_log(
        "[Critical] CPU does not support invariant TSC "
        "(constant_tsc + nonstop_tsc). rdtsc() is unreliable.\n"
        "Check: grep -E 'constant_tsc|nonstop_tsc' /proc/cpuinfo\n");
    std::abort();
  }
}

} // namespace mk::sys
