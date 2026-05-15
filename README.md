# Low-Latency Tick-to-Trade

Low-latency C++ trading system and component library for HFT-style environments.

Linux x86-64 only. C++20, clang++, zero cross-platform fallbacks.

## What This Is

1. **Tick-to-Trade Pipeline** (`apps/tick_to_trade/`) -- End-to-end trading system: UDP multicast market data, SPSC queue between threads, strategy evaluation, TCP order entry. Hot-path two-thread architecture plus an async logger thread. RDTSC instrumentation at key stage boundaries (always-on queue-wait + sent-order tick-to-trade; per-stage breakdown via `PROFILE_STAGES`).

2. **Multi-Process Simulated Exchange** (`apps/simulated_exchange/`) -- Industry-style exchange with 3 process types: Engine (matching, shared memory queue polling), per-client Gateway (recv/send thread separation), and Market Data Publisher (UDP multicast). Communicates via per-gateway SPSC queue pairs in POSIX shared memory. Supports cross-gateway fills, duplicate order ID detection, typed overload reject, and slot claim via atomic CAS.

3. **Reusable Low-Latency Components** (`libs/`) -- Header-only and static libraries, tested with Google Test and benchmarked with custom rdtsc + Google Benchmark. Zero-allocation hot paths, lock-free data structures, NUMA-aware memory management.

## Architecture

For system architecture, threading model, wire protocol, risk management, and memory layout, see [ARCHITECTURE.md](ARCHITECTURE.md).

**Measured latency** — Tick-to-Trade: **14 µs p50 / 47 µs p99** (sent orders, two-machine isolated LAN, Release build, Intel Coffee Lake, turbo off, isolated cores, zero queue drops, n=338k sent orders):

| Stage | p50 | p99 | Sample population |
|-------|-----|-----|-------------------|
| **Tick-to-Trade** | **14 µs** | **47 µs** | sent orders only |
| Feed Parse | 107 ns | 213 ns | all ticks |
| Queue Wait* | 2.6 µs | 32 µs | all ticks |
| Strategy | 53 ns | 213 ns | all ticks |
| Order Send | 8.4 µs | 22 µs | sent orders only |
| Order RTT | 582 µs | 1.1 ms | sent orders only |

\* Queue Wait is a representative single-run value, more sensitive to Strategy hot-loop ordering (drain cadence, `epoll_wait(0)`, order-send blocking) than the order-send path. In two collected runs, Queue Wait p50 moved by a few hundred nanoseconds while Tick-to-Trade and Order Send stayed similar.

Measured with `PROFILE_STAGES` enabled for per-stage attribution. Measurement methodology and Queue Wait semantics in [ARCHITECTURE.md — Measured Results](ARCHITECTURE.md#measured-results).

## Component Highlights

Median (p50) from rdtsc microbenchmarks (`bench/`, Release build, pinned to isolated core, representative run, 10k iterations).

| Component | Description | Median (p50) | p99 |
|-----------|-------------|-------------|-----|
| `ObjectPool<T>` | Fixed-size pool, pluggable free-list | 2.5ns (SingleThread alloc) / 25.8ns (LockFree alloc) | 4.2ns / 30.8ns |
| `SPSCQueue<T>` | Lock-free SPSC ring buffer, monotonic indices | 3.3ns push+pop | 5.0ns |
| `LockFreeStack<T>` | Treiber stack, 128-bit CAS (CMPXCHG16B), ABA-safe | 47.5ns push+pop | 51.7ns |
| `OrderBook` | Price-time priority, intrusive lists, zero allocation | 46.7ns add_order (new level) | 110ns |
| `MatchingEngine` | Crossing logic on top of OrderBook | 110ns submit_order (5 fills) | 156ns |
| Pipeline log enqueue | `log_market_data` (hot path: LogEntry construct + SPSC push) | 15.8ns | 20ns |
| `signal_log()` | Signal-safe, zero-allocation, `write(2)` based | 400.8ns per call | 522.5ns |
| `WireWriter/Reader` | Bounded cursor codec with endian conversion | 1.7ns (5 fields) | 5ns |
| `PackedCodec` | Zero-copy memcpy codec for same-arch IPC | 1.7ns per struct | 4.2ns |

All nanosecond figures are cache-hot steady-state medians from isolated-core microbenchmarks and should not be interpreted as end-to-end production latency.

Cold-path components (not benchmarked — cost is irrelevant to trading latency):

| Component | Description |
|-----------|-------------|
| `MmapRegion` | RAII mmap wrapper, huge pages, NUMA, prefault |
| `EpollWrapper` | RAII epoll with fd-based and pointer-based dispatch |

## Project Structure

```
libs/                 Reusable low-latency components
  sys/memory/         MmapRegion, ObjectPool, SPSC queues, lock-free stack
  sys/log/            Signal-safe logger, async logger (per-producer SPSC)
  sys/thread/         CPU affinity, hot-path allocation guard
  sys/                NanoClock (RDTSC), endian utils, bit utils
  ds/                 IntrusiveList, HashMap, RingBuffer, TimingWheel, IndexFreeStack
  net/                Socket hierarchy (TCP/UDP), EpollWrapper, wire codecs
  algo/               OrderBook (price-time priority), MatchingEngine

apps/                 Application binaries
  tick_to_trade/      Hot-path two-thread trading pipeline (+ async logger thread)
  simulated_exchange/ Multi-process exchange: Engine + N Gateways + MD Publisher
                      (shared memory SPSC IPC, per-client Gateway, recv/send threads)
  shared/             Wire protocol definitions (UDP market data, TCP orders)

tests/                Google Test suites mirroring libs/ and apps/
bench/                Benchmarks (custom rdtsc + Google Benchmark)
```

## Key Design Decisions

**Why `mmap` instead of `malloc`?**
Hot-path memory comes from `mmap` with explicit huge pages (2MB MAP_HUGETLB) or THP fallback. This greatly reduces TLB misses, enables NUMA binding, and allows prefaulting to remove page faults from the critical path.

**Why SPSC queues instead of lock-free MPSC?**
The pipeline has exactly one producer and one consumer per queue. SPSC needs only `acquire`/`release` ordering (single-digit ns push+pop). MPSC would require CAS that we do not need for this topology.

**Why non-owning data structures?**
`SPSCQueue<T>`, `HashMap<K,V>`, `RingBuffer<T>`, `IndexFreeStack` -- all take a caller-supplied buffer. This separates data structure logic from memory ownership, allowing the same code to work on huge pages, shared memory, or stack arrays.

**Why intrusive data structures?**
`IntrusiveList` eliminates per-node heap allocation. Order book price levels and order queues use intrusive lists for O(1) insert/remove with zero allocation, critical for maintaining sub-microsecond add_order latency.

## Build

Requires: Linux x86-64, clang++, CMake 3.16+

```bash
cmake --preset dev            # Debug build -> build/
cmake --build build -j$(nproc)
```

Other presets:

```bash
cmake --preset reldbg         # RelWithDebInfo (perf profiling)
cmake --preset release        # Release (NDEBUG, guards off)
cmake --preset dev-asan       # AddressSanitizer + UBSan
cmake --preset dev-tsan       # ThreadSanitizer
cmake --preset dev-tidy       # clang-tidy (zero warnings)
```

## Test

```bash
ctest --test-dir build --output-on-failure

# Single test binary
./build/tests/libs/sys/spsc_queue_test

# Specific test case
./build/tests/libs/sys/spsc_queue_test --gtest_filter="SPSCQueueStress.*"
```

## Benchmark

```bash
# Release for steady-state numbers (matches measurement in README table)
cmake --preset release
cmake --build build-release -j$(nproc)

# Custom rdtsc benchmark (min/median/p99/max)
taskset -c 2 ./build-release/bench/libs/algo/order_book_bench

# Google Benchmark (CI-friendly, JSON output)
./build-release/bench/libs/algo/order_book_gbench --benchmark_format=json
```

For `perf` profiling (flamegraph, stack unwinding), use `--preset reldbg` instead — optimized build (`-O2 -march=native + ThinLTO`) with debug symbols and frame pointer preserved.

## Run

The simulated exchange runs as 3 separate processes communicating via POSIX shared memory. Each Gateway serves exactly one client.

```bash
# Terminal 1: Exchange Engine (must start first — creates shared memory)
./build/apps/simulated_exchange/exchange_engine \
    --num_gateways=1 --shm_name=/mk_exchange_events --symbol_count=2

# Terminal 2: Order Gateway (waits for engine_ready)
./build/apps/simulated_exchange/exchange_gateway \
    --gateway_id=0 --tcp_port=8888 --shm_name=/mk_exchange_events

# Terminal 3: Trading Pipeline (joins multicast group before MD Publisher starts)
./build/apps/tick_to_trade/tick_to_trade \
    --exchange_host=127.0.0.1 --exchange_port=8888 \
    --mcast_group=239.255.0.1 --mcast_port=9000 \
    --pin_core_md=1 --pin_core_strategy=2 --pin_core_logger=3

# Terminal 4: Market Data Publisher (start last — pipeline is already listening)
./build/apps/simulated_exchange/exchange_md_publisher \
    --shm_name=/mk_exchange_events \
    --mcast_group=239.255.0.1 --mcast_port=9000
```

Core pinning flags (`--pin_core_*`) are optional — omit them to run without pinning. For low-jitter operation, see the system tuning stack described in [ARCHITECTURE.md](ARCHITECTURE.md).

## CI

GitHub Actions runs five jobs on pushes to `main` and pull requests targeting `main`:
- **build-and-test**: Debug build + full test suite
- **asan-ubsan**: AddressSanitizer + UBSan (memory safety)
- **release-build-and-test**: Release build + tests (`-O3 -march=native` + ThinLTO) — exercises compiler reordering that Debug masks
- **tsan**: ThreadSanitizer (data-race detection)
- **clang-tidy**: Zero warnings enforced (`WarningsAsErrors: '*'`)

## License

[MIT](LICENSE)
