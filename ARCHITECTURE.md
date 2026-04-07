# Architecture

> This document explains the design decisions behind the system.
> For system overview, component list, and build instructions, see [README.md](README.md).

---

## System Overview

### tick_to_trade

`tick_to_trade` runs 4 runtime threads:

- Main / launcher thread: cold-path setup, socket creation, memory allocation, thread lifecycle, shutdown summary
- MD Feed thread: UDP recv + parse + SPSC enqueue
- Strategy thread: SPSC dequeue + strategy + risk + TCP order send/recv
- Async Logger thread: drains per-thread log queues and writes `pipeline.log`

```
                                          ┌═══ tick_to_trade (process) ═════════════════════════════┐
                                          │  ┌──────────────────────────┐                           │
                                          │  │  Main / Launcher Thread  │                           │
                                          │  │  config, memory alloc,   │                           │
                                          │  │  socket setup, shutdown  │                           │
                                          │  └──┬─────────┬─────────┬───┘                           │
  ┌═══ md_publisher ════┐                 │     │         │         │                               │
  │                     │                 │spawn│    spawn│    spawn│                               │
  │                     │                 │     ▼         ▼         │                               │
  │ shm event poll      │   UDP multicast │ ┌────────────────────┐  │  ┌────────────────────────┐   │
  │ UDP multicast pub   │   (36B/dgram)   │ │ MD Feed Thread     │  │  │ Async Logger Thread    │   │
  │                     ├────────────────►│ │(--pin_core_md)     │  │  │(--pin_core_logger)     │   │
  │                     │                 │ │                    │  │  │                        │   │
  │                     │                 │ │ recvmmsg()×64      │  │  │ drain SPSC log         │   │
  │                     │                 │ │ FeedHandler parse  │  │  │  queues (×2)           │   │
  └═════════════════════┘                 │ │ spsc.try_push()    │  │  │ TSC-ordered merge      │   │
                                          │ │ rdtsc: t0 [,t1]    │  │  │ write(2) → pipeline.log│   │
  ┌═ exchange_gateway ══┐                 │ └─┬──────────────┬───┘  │  │                        │   │
  │ ┌───────┐ ┌───────┐ │                 │   │              │      │  │ (off critical path)    │   │
  │ │recv T.│ │send T.│ │                 │   │market        │      │  └────────────────────────┘   │
  │ └───────┘ └───────┘ │                 │   │data SPSC     │      │               ▲  ▲            │
  │ TCP accept, 1 client│                 │   │              │      │    log SPSC   │  │            │
  │ shm request/response│                 │   ▼              └──────│───────────────┘  │            │
  │ frame parse/build   │                 │ ┌─────────────────────┐ │                  │            │
  │                     │────────────────►│ │ Strategy Thread     │◄┘                  │            │
  │ heartbeat, throttle │                 │ │(--pin_core_strategy)│                    │            │
  │                     │       TCP       │ │                     │                    │            │
  │                     │                 │ │ spsc.drain()×64     │      log SPSC      │            │
  │                     │◄────────────────│ │  SpreadStrategy     ├────────────────────┘            │
  │                     │                 │ │  RiskCheck (7)      │                                 │
  └═════════════════════┘                 │ │  OrderManager       │                                 │
                                          │ │  TCP send/recv      │                                 │
                                          │ │ rdtsc: td [,t2,t3]  │                                 │
                                          │ │         t4          │                                 │
                                          │ └─────────────────────┘                                 │
                                          └═════════════════════════════════════════════════════════┘
```

The trading critical path is the MD Feed thread + Strategy thread. The async logger thread drains per-producer SPSC log queues from both hot-path threads, merges entries by TSC timestamp, and writes to disk via `write(2)`. It is not on the latency-sensitive path.

**Why two threads, not one?**
UDP `recvmmsg()` and TCP send/recv are on different sockets with different wake patterns. Separating them eliminates head-of-line blocking: the MD thread never stalls waiting for TCP, and the strategy thread is less likely to miss market data while sending orders.

**Why not three threads (separate OE gateway)?**
The current strategy logic and TCP order sending are fast enough on a single core. Adding a third thread would add another queue hop and complexity. The design supports this extension if profiling shows TCP syscall cost dominates.

**Core pinning:**
Thread pinning is optional. The MD and Strategy threads can be pinned via `pthread_setaffinity_np()` to isolated cores (`isolcpus`) using `--pin_core_md` and `--pin_core_strategy`. The async logger can also be pinned with `--pin_core_logger`, but it is off the trading critical path. Leaving a pin flag at `-1` disables pinning for that thread.

**Low-jitter deployment:**
Pinning alone is not sufficient. The deployment also assumes `performance` CPU governor, turbo boost disabled, `isolcpus` + `nohz_full` + `rcu_nocbs` (kernel boot parameters), manual NIC IRQ affinity, disabled C-states on isolated cores, and `timer_migration=0`.

**NUMA binding:**
Memory regions are bound to a NUMA node selected by priority: (1) explicit `--numa_node` override, (2) NIC node from sysfs (`--nic_iface`), (3) strategy core's node, (4) MD core's node, (5) no binding.

### simulated_exchange

The simulated exchange runs as 3 separate process types communicating via POSIX shared memory SPSC queues:

```
┌────────────────────────────────────────────────────────────────────────────────┐
│                          /dev/shm/mk_exchange_events                           │
│                                                                                │
│  ┌──────────────────────────────────────────────────────────────────────────┐  │
│  │  Per-gateway queue pairs (kMaxGateways = 8):                             │  │
│  │   request_queues[0]  response_queues[0]  ← Gateway 0                     │  │
│  │   request_queues[1]  response_queues[1]  ← Gateway 1                     │  │
│  │   ...                                                                    │  │
│  └──────────────────────────────────────────────────────────────────────────┘  │
│                                                        ┌────────────────────┐  │
│                            shared (single Publisher) → │ md_event_queue     │  │
│                                                        └────────────────────┘  │
│   engine_ready | shutdown | num_gateways | symbol_count | gateway_claimed[8]   │
└────────────────────────────────────────────────────────────────────────────────┘
              ▲▼                           ▲▼                        ▲▼
┌═══════════════════════════┐  ┌══════════════════════┐  ┌═══════════════════════┐
│ exchange_gateway × N      │  │ exchange_engine      │  │ exchange_md_publisher │
│ (1 process per client)    │  │ (single thread)      │  │ (single thread)       │
│                           │  │                      │  │                       │
│  ┌─────────────────────┐  │  │ - ExchangeCore       │  │ - Publisher           │
│  │ recv thread:        │  │  │ - MatchingEngine     │  │   × N symbols         │
│  │ accept + parse      │  │  │   × N symbols        │  │                       │
│  │ shm request push    │  │  │                      │  │ shm event polling +   │
│  └─────────────────────┘  │  │ Round-robin polls    │  │ timer-based ticks     │
│  ┌─────────────────────┐  │  │ N request queues.    │  │ UDP multicast send    │
│  │ send thread:        │  │  │ Routes responses by  │  │                       │
│  │ shm response pop    │  │  │ session_to_gateway[].│  │                       │
│  │ serialize + send    │  │  │                      │  │                       │
│  └─────────────────────┘  │  │                      │  │                       │
└═══════════════════════════┘  └══════════════════════┘  └═══════════════════════┘
           │     ▲                                                   │
responses  │ TCP │ orders                                            │
(Ack/Fill) │     │ (NewOrder etc)                                    │
           ▼     │                                                   │
┌═══════════════════════════┐                      UDP (one-way)     │
│ tick_to_trade (per client)│ ◄──────────────────────────────────────┘
└═══════════════════════════┘
```

- **exchange_engine**: Matching engine + session management. No network I/O. Round-robin polls per-gateway request queues, routes responses by `session_to_gateway` mapping (including cross-gateway fills).
- **exchange_gateway** (× N, one per client): recv thread (accept + parse + request queue push) + send thread (response queue pop + serialize + TCP send). Single-writer guarantee — all TCP writes go through the send thread.
- **exchange_md_publisher**: Reads fill and BBO events from shared memory, publishes trade and BBO datagrams via UDP multicast (distinguished by `md_msg_type` field). Also generates synthetic quotes at fixed intervals.

**Why separate processes?** Isolating the Engine from network I/O means matching latency is not directly coupled to TCP/UDP syscall delays. Each runtime thread can be pinned to its own core independently.

**Why shared memory IPC?** Same-machine inter-process communication via shared memory SPSC queues avoids the kernel network stack overhead of TCP loopback (order-of-magnitude difference: ~50 ns vs ~10 µs, illustrative).

---

## Data Flow

Instrumented metrics at a glance:
- **Always-on**: `queue_hop`, `queue_wait`, `tick_to_trade` (sent orders only)
- **Optional** (`PROFILE_STAGES`): `feed_parse`, `strategy_eval`, `order_send`

A single market data tick traverses up to 7 instrumented rdtsc points. Three are always recorded: `t0` (post-recv), `t_drain` (post-batch-drain), and `td` (per-item processing). `t4` (post-TCP-send) and `tick_to_trade` are recorded only when an order is actually sent. Three more points are added when `PROFILE_STAGES` is enabled: `t1` (post-parse), `t2` (post-strategy), and `t3` (pre-order-send).

```
UDP datagram arrives
        │
        ▼
┌─ MD Feed Thread ──────────────────────────────────┐
│                                                   │
│  recvmmsg(fd, batch=64, MSG_DONTWAIT)             │
│  t0 = rdtsc()          ← post-recv userspace ts   │
│                                                   │
│  FeedHandler::on_udp_data()                       │
│    ├─ parse 36-byte datagram → MarketDataUpdate   │
│    ├─ sequence gap detection (per-feed)           │
│    └─ duplicate filtering                         │
│  t1 = rdtsc()          ← [PROFILE_STAGES only]    │
│                                                   │
│  spsc.try_push(QueuedUpdate{update, t0})          │
└───────────────────────┬───────────────────────────┘
                        │
              SPSCQueue<QueuedUpdate>
              (runtime capacity 1024, MmapRegion-backed)
                        │
                        ▼
┌─ Strategy Thread ─────────────────────────────────┐
│                                                   │
│  spsc.drain(batch, 64)                            │
│  t_drain = rdtsc()     ← batch drain timestamp    │
│  for each item:                                   │
│    td = rdtsc()        ← per-item timestamp       │
│                                                   │
│  SpreadStrategy::on_market_data()                 │
│    ├─ update per-symbol BBO                       │
│    └─ signal if spread > threshold                │
│  t2 = rdtsc()          ← [PROFILE_STAGES only]    │
│                                                   │
│  OrderSendHandler::on_signal()                    │
│  t3 = rdtsc()          ← [PROFILE_STAGES only]    │
│    ├─ OrderManager: 7-stage risk check            │
│    ├─ serialize → scratch buffer                  │
│    ├─ pack_tcp_message() (header + payload)       │
│    └─ tcp_sock.send_nonblocking()                 │
│  t4 = rdtsc()          ← [on order send only]     │
└───────────────────────────────────────────────────┘

Latencies tracked:
  feed_parse    = t1 - t0      (FeedHandler)              [PROFILE_STAGES]
  queue_hop     = td - t0      (per-item, includes batch) [always-on]
  queue_wait    = t_drain - t0 (post-recv to batch drain) [always-on]
  strategy_eval = t2 - td      (signal generation)        [PROFILE_STAGES]
  order_send    = t4 - t3      (risk + serialize + TCP)   [PROFILE_STAGES]
  tick_to_trade = t4 - t0      (end-to-end, sent orders)  [on order send only]
```

---

## Wire Protocol

Two independent protocols, chosen for different performance requirements.

### UDP Market Data (36 bytes, no framing)

One datagram = one update. No TLV header — the datagram boundary is the frame. The `md_type` field distinguishes BBO updates (quotes) from trades.

```
 0       8      12  13  14  16      24      28       36
 ┌───────┬───────┬───┬───┬───┬───────┬───────┬────────┐
 │seq_num│sym_id │typ│sid│pad│ price │  qty  │exch_ts │
 │ u64   │ u32   │u8 │u8 │2B │ i64   │ u32   │  i64   │
 └───────┴───────┴───┴───┴───┴───────┴───────┴────────┘
  typ: 0 = BBO update, 1 = Trade
```

**Why no TLV header?** Market data is latency-critical and fixed-format. A single bounds check at the start, then unchecked field reads — no per-field validation overhead. Datagram boundaries guarantee message atomicity (no partial reads).

### TCP Orders (fixed-header framing)

All TCP messages use a 16-byte header followed by variable-length payload:

```
 0       4     6     8      12     16
 ┌───────┬─────┬─────┬───────┬──────┬─────────────┐
 │ magic │ ver │type │pay_len│flags │  payload... │
 │  4B   │ 2B  │ 2B  │  4B   │  4B  │   N bytes   │
 └───────┴─────┴─────┴───────┴──────┴─────────────┘
 All fields big-endian (network byte order)
```

**Message types:**

| Type | Direction | Payload |
|------|-----------|---------|
| NewOrder | pipeline → exchange | client_order_id, symbol_id, side, price, qty, send_ts (33B) |
| CancelOrder | pipeline → exchange | client_order_id, symbol_id, send_ts (20B) |
| ModifyOrder | pipeline → exchange | client_order_id, symbol_id, new_price, new_qty, send_ts (32B) |
| OrderAck | exchange → pipeline | client_order_id, exchange_order_id, send_ts (24B) |
| OrderReject | exchange → pipeline | client_order_id, reject_reason, send_ts (17B) |
| FillReport | exchange → pipeline | client_order_id, exchange_order_id, fill_price, fill_qty, remaining_qty, send_ts (40B) |
| CancelAck | exchange → pipeline | client_order_id, send_ts (16B) |
| CancelReject | exchange → pipeline | client_order_id, reject_reason, send_ts (17B) |
| ModifyAck | exchange → pipeline | client_order_id, new_exchange_order_id, send_ts (24B) |
| ModifyReject | exchange → pipeline | client_order_id, reject_reason, send_ts (17B) |
| Heartbeat | pipeline → exchange | header only (no payload) |
| HeartbeatAck | exchange → pipeline | header only (no payload) |

**Why a header for TCP but not UDP?** TCP is a byte stream with no message boundaries. The fixed header provides framing (length-prefixed) and message type discrimination. The overhead is acceptable — order messages are cold-path relative to market data (orders per second << ticks per second).

---

## Risk Management

Every order passes 7 sequential checks before reaching the wire. All checks are O(1), zero allocation.

```
Signal from Strategy
        │
        ▼
  1. Kill switch active?           → block
  2. Global outstanding ≥ max?     → reject
  3. Per-symbol outstanding ≥ max? → reject
  4. Order qty > max_order_size?   → reject
  5. Notional > max_notional?      → reject
  6. Token bucket empty?           → reject  (rate limiter)
  7. Position ≥ max_position?      → reject
        │
        ▼
  NewOrder → TCP send
```

**Kill switch** (triggered via first SIGINT/SIGTERM, or explicitly via `SIGUSR1`):
State machine `kNormal → kCancelling → kDraining → kComplete`. Cancels all in-flight orders, blocks new ones, waits for ack/reject on every outstanding order before declaring complete.

**Order timeouts:**
`TimingWheel` with 1ms tick granularity. Orders still outstanding after `--order_timeout_ms` (default 5s) are collected into a batch buffer via callback, then cancelled by the strategy thread on the next loop iteration. `OrderAck` does not cancel the timer. Timers are cleared when the order leaves outstanding state via reject, cancel ack/reject, or complete fill (`remaining_qty == 0`).

**Connection health:**
Heartbeat every 1s, timeout at 3s. Startup uses blocking `connect()` (cold path, once). Reconnection uses non-blocking `connect()` via epoll with 3s deadline and exponential backoff (1s → 10s max, 10 attempts).

---

## Strategy

The pipeline uses compile-time strategy polymorphism via a `StrategyPolicy` concept. The active strategy is selected by a type alias in `main.cpp` — swapping strategies requires no virtual dispatch, no vtable, and enables full inlining.

The default strategy is `SpreadStrategy<NSymbols>` (currently `NSymbols=2`):

- Tracks per-symbol BBO (best bid, best ask) from market data updates
- Generates a signal when both sides are present and `spread > --spread_threshold`
- Signal side alternates buy/sell per symbol to avoid directional bias
- Signal price is aggressive: buy at the ask, sell at the bid (crosses the spread)
- Order quantity is fixed at `--order_qty`

This is intentionally simple — the project demonstrates infrastructure, not alpha. A production strategy would add book depth, queue position sensing, and adverse selection detection.

---

## Shutdown

Two-stage signal handling provides graceful shutdown:

```
SIGINT/SIGTERM (1st) or SIGUSR1
        │
        ▼
  g_kill_switch flag set
        │
        ▼
  Strategy thread: trigger_kill_switch()
    ├─ Cancel all outstanding orders
    ├─ Block new orders
    └─ Wait for CancelAck/CancelReject on each
        │
        ▼
  All cancel responses received → g_stop
        │
        ▼
  SIGINT/SIGTERM (2nd) — force stop (skips drain)
        │
        ▼
  All threads exit, main joins, final stats dump
```

The first SIGINT/SIGTERM (or SIGUSR1) triggers the kill switch — outstanding orders are cancelled gracefully. A second SIGINT/SIGTERM forces immediate exit. This matches production trading system patterns where an operator can escalate from graceful to forced shutdown.

The Strategy thread can also initiate shutdown autonomously by setting `g_stop` directly:
- Kill switch drain complete (all cancel responses received)
- TCP reconnection attempts exhausted (`kMaxReconnectAttempts`)

---

## Memory Architecture

All hot-path memory is allocated once at startup. Components receive non-owning `void*` pointers — they never allocate.

### Why Not ObjectPool?

`ObjectPool<T>` bundles `MmapRegion` (allocation) + `FreeList` (slot management). The pipeline separates these concerns:

| | ObjectPool | Pipeline approach |
|---|---|---|
| Allocation | Built-in MmapRegion | Centralized in `main.cpp` |
| Slot management | LockFreeStack + Node wrapper | IndexFreeStack (no wrapper) |
| Per-object overhead | sizeof(Node) wrapper | Zero |
| Thread safety | Configurable | Single-threaded by design |

`ObjectPool` is a reusable building block in `libs/` for concurrent producers/consumers. The pipeline's components are single-threaded and benefit from the lighter `IndexFreeStack`.

### Buffer Layout

`main.cpp` allocates 4 startup regions:

```
Region 1: StrategyCtx (single contiguous buffer, pointer-carved)
  ├─ TCP RX buffer           (cache-line aligned)
  ├─ TCP TX buffer           (cache-line aligned)
  ├─ Scratch buffer          (order serialization)
  ├─ Net position array      (per-symbol)
  ├─ Outstanding per-symbol  (per-symbol counters)
  ├─ Active orders array     (per-symbol × 2 sides)
  ├─ Cancel batch buffer     (max_outstanding entries)
  ├─ Outstanding HashMap     (order tracking, carved from same buffer)
  ├─ Timeout IDs array       (max_outstanding entries)
  └─ TimingWheel buffer      (order timeouts)

Region 2: MdCtx (MD thread buffers)
  ├─ recvmmsg scatter-gather arrays
  └─ 64 × 64-byte datagram buffers

Region 3: SPSC Queue
  └─ 1024 × QueuedUpdate (48B each)

Region 4: LatencyTracker
  └─ Histogram storage (~66KB)
```

All regions: two-level huge page fallback (MAP_HUGETLB → THP), NUMA-bound via `mbind()` when a node is resolved, prefaulted via `MADV_POPULATE_WRITE`. `mlockall(MCL_CURRENT | MCL_FUTURE)` is attempted after all allocations — failure is logged as a warning and execution continues.

### The Pattern

```
┌─────────────────────────────────────────────────┐
│  main.cpp (cold path)                           │
│                                                 │
│  1. allocate_hot_rw_region() → MmapRegion       │
│  2. Configure: NUMA bind, prefault, mlock       │
│  3. Pass raw pointer to component               │
│                                                 │
│  MmapRegion lifetime = program lifetime         │
│  Component lifetime ⊆ MmapRegion lifetime       │
└─────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────┐
│  Component (hot path, zero allocation)          │
│                                                 │
│  - Receives void* buf, size_t buf_size          │
│  - Never calls mmap, malloc, new                │
│  - static required_buffer_size() for caller     │
└─────────────────────────────────────────────────┘
```

Zero allocation on the hot path is verified by a Debug-mode global `new`/`delete` guard that aborts on any heap allocation from a hot-path thread.

---

## Latency Measurement

### Always-On

Always-on measurement keeps three metrics enabled without rebuild: `queue_hop` (`td - t0`), `queue_wait` (`t_drain - t0`), and sent-order `tick_to_trade` (`t4 - t0`). `queue_wait` measures post-recv to batch drain — the interval data spends in transit (parse, SPSC push, cross-thread scheduling delay, Strategy loop overhead). `queue_hop` adds per-item batch processing accumulation on top of `queue_wait`. This gives continuous visibility into the MD-to-Strategy handoff and outbound order-path latency at lower overhead than full stage profiling.

### Compile-Time: Per-Stage Breakdown

Enabled via the `PROFILE_STAGES` CMake option. Adds `feed_parse`, `strategy_eval`, and `order_send` breakdowns. Used for diagnosing which stage causes latency spikes. Zero overhead when disabled. Measured instrumentation overhead: localhost Tick-to-Trade p50 improved from 6.7 µs to 6.5 µs when `PROFILE_STAGES` was disabled (see Key observations below).

| Approach | Overhead | Rebuild needed? | Use case |
|----------|----------|-----------------|----------|
| Always-on queue-hop + queue-wait + sent-order tick-to-trade | low | No | Continuous runtime monitoring |
| Compile-time per-stage | ~0.2 µs (measured) | Yes | Latency diagnosis |
| Runtime flag (`atomic<bool>`) | ~1ns branch | No | On-demand profiling |

This project uses the first two. The runtime flag approach avoids rebuilds but adds a branch to every tick — acceptable for most systems, avoided by the most latency-sensitive ones.

### Measured Results

All numbers are steady-state (excluding first 256 warm-up ticks). `PROFILE_STAGES` enabled. Release build (`cmake --preset release`). Multi-process exchange (3 processes). Two-machine numbers are from a representative run.

**Test environment:**
- Trading server: Intel Coffee Lake 8C/16T (2.4 GHz fixed, turbo off), 32 GB, Linux 6.19.7
- Exchange server: Intel Coffee Lake 6C/12T (2.6 GHz fixed, turbo off), 16 GB, Linux 6.19.8
- Both: `performance` governor, C-states disabled on isolated cores, `timer_migration=0`, `irqbalance` disabled
- Trading server: `isolcpus=1-3,9-11`, `nohz_full`, `rcu_nocbs` — MD on core 1, Strategy on core 2, AsyncLogger on core 3
- Exchange server: `performance` governor, no additional tuning (exchange latency is not measured)
- Network: Gigabit Ethernet switch, isolated LAN (no internet traffic during measurement)
- Simulated exchange tick interval: 100 µs (~10k ticks/sec)

**Localhost** (exchange + pipeline on same machine, Release build) — Tick-to-Trade **6.7 µs p50**:

| Stage | p50 | p99 | p999 | Sample population |
|-------|-----|-----|------|-------------------|
| Feed Parse | 107 ns | 213 ns | 213 ns | all ticks |
| Queue Wait | 587 ns | 6.8 µs | 13 µs | all ticks |
| Queue Hop | 640 ns | 6.9 µs | 13.1 µs | all ticks |
| Strategy | 107 ns | 213 ns | 267 ns | all ticks |
| Order Send | 5.8 µs | 7.8 µs | 9.9 µs | sent orders only |
| Order RTT | 222 µs | 347 µs | 580 µs | sent orders only |
| **Tick-to-Trade** | **6.7 µs** | **12.7 µs** | **19.1 µs** | sent orders only |

**Two-machine** (exchange server → switch → trading server, isolated LAN, representative run) — Tick-to-Trade **11.8 µs p50**:

| Stage | p50 | p99 | p999 | Sample population |
|-------|-----|-----|------|-------------------|
| Feed Parse | 160 ns | 267 ns | 320 ns | all ticks |
| Queue Wait | 853 ns | 21 µs | 38 µs | all ticks |
| Queue Hop | 1.0 µs | 22.8 µs | 54.6 µs | all ticks |
| Strategy | 107 ns | 213 ns | 320 ns | all ticks |
| Order Send | 10.1 µs | 15.9 µs | 54 µs | sent orders only |
| Order RTT | 619 µs | 1.19 ms | — | sent orders only |
| **Tick-to-Trade** | **11.8 µs** | **28.8 µs** | **54.6 µs** | sent orders only |

Order RTT p999 is omitted — values above 4 ms overflow the histogram's last bucket. Raw max can extend far beyond the histogram range due to timeout/cancel lifecycle outliers and is not representative of median-path network RTT.

**Key observations:**

#### Interpretation

- **Feed Parse and Strategy are network-independent** (~100-300 ns). These measure pure CPU work — parsing a fixed 36-byte datagram and evaluating a spread condition. Consistent across localhost and two-machine setups.
- **Queue Wait and Queue Hop** both start at `t0` (post-recv, before parse) but end at different points:

  ```
  MD Thread:
    recvmmsg()
    t0 = rdtsc()           ← both metrics start here
    parse() + spsc.push()

  Strategy Thread:
    drain(batch, 64)        ← dequeue up to 64 items
    t_drain = rdtsc()       ← Queue Wait ends here (once per drain)

    for each item in batch:
      td = rdtsc()          ← Queue Hop ends here (per item)
      process(item)         ← strategy eval + possible order send
  ```

  **Queue Wait** (`t_drain - t0`): how long data sat in transit — parse, SPSC push, cross-thread scheduling delay, and Strategy loop overhead until `drain()` is called. All items in the same batch share the same `t_drain`, so batch-internal processing time is excluded.

  **Queue Hop** (`td - t0`): Queue Wait plus per-item batch processing accumulation. Later items in a batch include earlier items' processing time in their Queue Hop.

  Neither is pure SPSC residence time — the component benchmark (~5 ns push+pop) measures that in isolation.

  **Why Queue Wait p99 is large** (~21 µs on two-machine): Queue Wait reflects how long it takes the Strategy thread to return to `drain()`. When the previous loop iteration involves order sends, TCP response processing, or timeout handling, the next `drain()` is delayed — and items pushed by the MD thread during that time accumulate longer Queue Wait values:

  ```
  MD thread:   push(A)  push(B)  push(C)  push(D)  push(E)
                 │        │        │        │        │
  Strategy:  ─── previous loop busy (order send + TCP response) ───
                                                     drain()
                                                     t_drain

  Queue Wait[A] = t_drain - t0[A] = large (pushed 25 µs ago)
  Queue Wait[E] = t_drain - t0[E] = small (pushed just now)
  ```

  Comparing localhost vs two-machine (Queue Wait p50: 587 ns → 853 ns, Queue Hop p50: 640 ns → 1.0 µs): the increase is consistent with longer Strategy thread loop cycles in the physical network environment (TCP send through NIC vs loopback), though the instrumentation does not isolate the exact cause. Network traversal time itself is not included (`t0` is post-recv).

#### Caveats

- **Sample populations differ across metrics.** Queue Wait/Hop are recorded on **every** market data update (all ticks). Tick-to-Trade is recorded **only** when an order or modify is actually sent. This means Queue Wait/Hop p50 reflects the typical loop cycle time across all ticks, while Tick-to-Trade p50 reflects only the order-sending path. At 100 µs tick interval (~10k ticks/sec) with ~100 orders/sec, Queue Wait/Hop has ~100× more samples than Tick-to-Trade, and most of those samples are from "no order sent" loops which are faster.

- **Tick interval affects Queue Wait/Hop but not Tick-to-Trade.** At 1000 µs tick interval, Queue Hop p50 rises to ~5 µs while Tick-to-Trade p50 remains ~12 µs. This is because `t0` is stamped after `recvmmsg()` returns to userspace — not when the packet arrives at the kernel. With a dense feed (100 µs), packets accumulate in the kernel socket buffer before `recvmmsg()` drains them in a batch; the kernel queuing time (`s0` to `t0`) is invisible to Queue Wait/Hop. With a sparse feed (1000 µs), packets rarely queue in the kernel, so `t0 ≈ s0` — but the Strategy loop idle time between ticks becomes visible in Queue Wait/Hop instead. With kernel bypass (e.g., OpenOnload), `t0` is stamped at userspace poll time, making it closer to the actual packet handoff regardless of tick rate.

#### Bottlenecks and improvements

- **Order Send dominates Tick-to-Trade on localhost** (~90% of p50). It includes TCP `send()` through the kernel network stack (syscall overhead, socket buffer copy, TCP state machine) — an inherent cost of kernel sockets. On two-machine, Order Send remains the largest single contributor (10.1 µs p50), with Queue Hop adding 1.0 µs.
- **Potential improvement with kernel bypass**: OpenOnload (Solarflare) via `LD_PRELOAD` replaces the kernel network stack with a userspace implementation. The socket API (`send`, `recv`, `epoll`) remains identical — no code changes required. This is expected to significantly reduce Order Send latency, though the exact improvement depends on hardware and has not been measured in this project.
- **p999 spikes to ~55 µs** are consistent with occasional queue backpressure and OS jitter (timer interrupts, TLB shootdowns), though the instrumentation does not isolate the exact cause. `idle=poll` (forcing all cores to stay in C0) would likely reduce these spikes but was not used in this test due to thermal constraints of the test hardware.
- **Instrumentation overhead** (localhost, Release): with `PROFILE_STAGES` disabled (only Tick-to-Trade, Queue Hop, and Queue Wait recorded), localhost sent-order Tick-to-Trade improved from 6.7 µs to 6.5 µs p50 (p99 unchanged at ~12.8 µs). The diagnostic per-stage instrumentation adds approximately 0.2 µs p50 in this setup — small in release builds due to optimized rdtsc inlining.
