#include "bench_utils.hpp"

#include <algorithm>
#include <cstdio>

namespace mk::bench {

Stats compute_stats(const sys::TscCalibration &cal,
                    std::span<std::uint64_t> data) {
  const auto overhead = cal.overhead_cycles;
  for (auto &d : data) {
    d = d > overhead ? d - overhead : 0;
  }
  std::sort(data.begin(), data.end());
  const auto n = data.size();
  return Stats{
      .min_ns = cal.to_ns(data[0]),
      .median_ns = cal.to_ns(data[n / 2]),
      .p99_ns = cal.to_ns(data[n * 99 / 100]),
      .max_ns = cal.to_ns(data[n - 1]),
  };
}

void print_header() {
  std::printf("  %-34s %8s %8s %8s %8s\n", "Operation", "min", "median", "p99",
              "max");
  std::printf("  %-34s %8s %8s %8s %8s\n", "──────────────────────────────────",
              "────────", "────────", "────────", "────────");
}

void print_stats(const char *name, const Stats &s) {
  std::printf("  %-34s %6.1fns %6.1fns %6.1fns %6.1fns\n", name, s.min_ns,
              s.median_ns, s.p99_ns, s.max_ns);
}

sys::TscCalibration calibrate_and_print() {
  sys::assert_tsc_reliable();

  std::printf("Calibrating TSC... ");
  std::fflush(stdout);
  auto cal = sys::TscCalibration::calibrate();
  std::printf("%.3f GHz (rdtsc overhead: %lu cycles = %.1f ns)\n",
              cal.freq_ghz(), cal.overhead_cycles,
              cal.to_ns(cal.overhead_cycles));
  return cal;
}

} // namespace mk::bench
