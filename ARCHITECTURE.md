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
  │ UDP multicast pub   │   (34B/dgram)   │ │ MD Feed Thread     │  │  │ Async Logger Thread    │   │
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

**Low-jitter deployment (recommended):**
Pinning alone is not sufficient. A production deployment is expected to also apply `performance` CPU governor, turbo boost disabled, `isolcpus` + `nohz_full` + `rcu_nocbs` (kernel boot parameters), manual NIC IRQ affinity, disabled C-states on isolated cores, and `timer_migration=0`. The measurement run in §Measured Results applies all of the above *except* manual NIC IRQ affinity (left to the kernel/irqbalance default, with irqbalance itself disabled).

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
- **Always-on**: `tick_to_trade` (kernel-timestamp-based, sent orders only), `queue_wait` (TSC-domain, all ticks); `queue_hop` is recorded as an internal per-item diagnostic
- **Optional** (`PROFILE_STAGES`): `feed_parse`, `strategy_eval`, `order_send`

A single market data tick is instrumented at two clock domains:
- **CLOCK_REALTIME ns-domain**: `t_kernel_rx` (kernel software RX timestamp from `SO_TIMESTAMPNS` cmsg) and `t_post_send_ns` (`clock_gettime(CLOCK_REALTIME)` after `send()` returns) drive the headline Tick-to-Trade. Also: `t_userspace_rx_ns` (`clock_gettime` immediately after `recvmmsg()`) used for the RX-kernel-path paired-delta validation.
- **TSC cycles**: `t0` (post-`recvmmsg()`), `t_drain` (post-batch-drain), `td` (per-item), `t4` (post-`send()`) drive the stage-breakdown timing and the TSC inner span cross-validation metric. Three more TSC points are added when `PROFILE_STAGES` is enabled: `t1` (post-parse), `t2` (post-strategy), `t3` (pre-order-send).

```
UDP datagram arrives at NIC
        │
        ▼  (NIC driver → kernel RX path: NAPI, sk_buff, socket queue)
        │
        ▼  t_kernel_rx ← kernel SW timestamp (SO_TIMESTAMPNS cmsg)
        │   ↑                                  ↓
        │   └── RX kernel path (~16 µs paired delta) ──┐
        │                                              │
┌─ MD Feed Thread ──────────────────────────────────┐  │
│                                                   │  │
│  recvmmsg(fd, batch=64, MSG_DONTWAIT)             │  │
│  t_userspace_rx_ns = clock_gettime(CLOCK_REALTIME)│◄─┘
│  t0 = rdtsc()          ← post-recv userspace ts   │
│                                                   │
│  for each datagram:                               │
│    parse SO_TIMESTAMPNS cmsg → t_kernel_rx        │
│    FeedHandler::on_udp_data()                     │
│      ├─ parse 34-byte datagram                    │
│      ├─ sequence gap detection (per-feed)         │
│      └─ duplicate filtering                       │
│    t1 = rdtsc()        ← [PROFILE_STAGES only]    │
│    spsc.try_push(QueuedUpdate{update, t0,         │
│                              t_kernel_rx})        │
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
│  t3 = rdtsc()          ← [PROFILE_STAGES only]    │◄─┐
│    ├─ OrderManager: 7-stage risk check            │  │ Order Send path
│    ├─ serialize → scratch buffer                  │  │ (~11.3 µs at p50:
│    ├─ pack_tcp_message() (header + payload)       │  │  risk + serialize
│    └─ tcp_sock.send_nonblocking()                 │  │  + TCP send path
│  t4 = rdtsc()          ← [on order send only]     │◄─┘  / socket queueing)
│  t_post_send_ns = clock_gettime(CLOCK_REALTIME)   │
└───────────────────────────────────────────────────┘

Latencies tracked:
  feed_parse      = t1 - t0                (FeedHandler)             [PROFILE_STAGES]
  queue_hop       = td - t0                (per-item, includes batch) [always-on]
  queue_wait      = t_drain - t0           (post-recv to batch drain) [always-on]
  strategy_eval   = t2 - td                (signal generation)        [PROFILE_STAGES]
  order_send      = t4 - t3                (risk + serialize + TCP)   [PROFILE_STAGES]
  tsc_inner_span  = t4 - t0                (TSC userspace boundary)   [always-on]
  rx_kernel_path  = t_userspace_rx_ns - t_kernel_rx   (paired delta) [always-on]
  tick_to_trade   = t_post_send_ns - t_kernel_rx      (HEADLINE)     [on order send only]
```

---

## Wire Protocol

Two independent protocols, chosen for different performance requirements.

### UDP Market Data (34 bytes, no framing)

One datagram = one update. No TLV header — the datagram boundary is the frame. The `md_type` field distinguishes BBO updates (quotes) from trades.

```
 0       8      12  13  14      22      26       34
 ┌───────┬───────┬───┬───┬───────┬───────┬────────┐
 │seq_num│sym_id │typ│sid│ price │  qty  │exch_ts │
 │ u64   │ u32   │u8 │u8 │ i64   │ u32   │  i64   │
 └───────┴───────┴───┴───┴───────┴───────┴────────┘
  typ: 0 = BBO update, 1 = Trade
  No padding — codec uses memcpy so wire alignment is not required.
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

Always-on measurement keeps four public metrics enabled without rebuild:

- `tick_to_trade` — headline metric: `t_post_send_ns - t_kernel_rx` (CLOCK_REALTIME ns-domain, sent orders only). Spans the kernel SW RX timestamp (`SO_TIMESTAMPNS` cmsg) to the post-`send()` return — closest to the wire-to-wire industry definition reachable without HW-timestamping NICs.
- `rx_kernel_path` — paired-delta validation: `t_userspace_rx_ns - t_kernel_rx` (same CLOCK_REALTIME ns-domain, all ticks). Directly measures the kernel socket RX/wakeup path that the headline metric captures, with no histogram-subtraction confound.
- `tsc_inner_span` — TSC-domain cross-validation: `t4 - t0` (rdtsc cycles, sent orders only). Narrower window than the headline (post-`recvmmsg()` → post-`send()`) but useful for stage attribution and as a fallback when `SO_TIMESTAMPNS` is unavailable.
- `queue_wait` — `t_drain - t0` (rdtsc cycles, all ticks). MD-to-Strategy SPSC handoff latency including parse, push, cross-thread scheduling, and Strategy-loop overhead until `drain()` is called.

A fifth value, `queue_hop` (`td - t0`), is recorded as an internal per-item diagnostic; it includes batch-position accumulation and is not a SPSC latency measurement. This gives continuous visibility across both clock domains at lower overhead than full stage profiling.

### Compile-Time: Per-Stage Breakdown

Enabled via the `PROFILE_STAGES` CMake option. Adds `feed_parse`, `strategy_eval`, and `order_send` breakdowns. Used for diagnosing which stage causes latency spikes. Adds extra rdtsc reads per tick — zero overhead when disabled.

| Approach | Overhead | Rebuild needed? | Use case |
|----------|----------|-----------------|----------|
| Always-on tick-to-trade + rx_kernel_path + tsc_inner_span + queue_wait | low | No | Continuous runtime monitoring |
| Compile-time per-stage | extra rdtsc per stage | Yes | Latency diagnosis |
| Runtime flag (`atomic<bool>`) | ~1ns branch | No | On-demand profiling |

This project uses the first two. The runtime flag approach avoids rebuilds but adds a branch to every tick — acceptable for most systems, avoided by the most latency-sensitive ones.

### Measured Results

All numbers are from a representative two-machine steady-state run (excluding first 256 warm-up ticks). `PROFILE_STAGES` enabled. Release build (`cmake --preset release`). Multi-process exchange (3 processes).

**Test environment:**
- **Trading server** (`tick_to_trade`): Intel Coffee Lake 8C/16T @ 2.4 GHz (turbo off), 32 GB, Linux 6.19.6, `isolcpus=1-3,9-11`. Pinning: MD on 1, Strategy on 2, AsyncLogger on 3.
- **Exchange server** (3-process exchange): Intel Coffee Lake 6C/12T @ 2.6 GHz (turbo off), 16 GB, Linux 6.19.6, `isolcpus=1,7`. Pinning: MD Publisher on 1; Engine/Gateway unpinned (latency not measured).
- **Both servers**: `performance` governor, `nohz_full` + `rcu_nocbs` on all isolated cores, C-states disabled, `timer_migration=0`, `irqbalance` disabled.
- **Network**: Gigabit Ethernet switch, isolated LAN.
- **Tick interval**: 100 µs (~10k ticks/sec).

**Two-machine** (exchange server → switch → trading server, isolated LAN, representative run with zero queue drops and zero sequence gaps, n=320k sent orders over ~210 s) — Tick-to-Trade **33.0 µs p50**:

| Stage | p50 | p99 | p999 | Sample population |
|-------|-----|-----|------|-------------------|
| Feed Parse | 107 ns | 213 ns | 267 ns | all ticks |
| RX kernel path\* | 16.1 µs | 33.5 µs | 88.1 µs | all ticks |
| Queue Wait\*\* | 640 ns | 20.2 µs | 33.4 µs | all ticks |
| Strategy | 107 ns | 213 ns | 267 ns | all ticks |
| Order Send path | 11.3 µs | 18.8 µs | 53.0 µs | sent orders only |
| TSC inner span\*\*\* | 13.5 µs | 35.1 µs | 54.6 µs | sent orders only |
| Order RTT | 606 µs | 1.17 ms | 4.10 ms | sent orders only |
| **Tick-to-Trade** | **33.0 µs** | **63.0 µs** | **170.0 µs** | sent orders only |

\* **RX kernel path** is a paired-delta measurement: `clock_gettime(CLOCK_REALTIME)` immediately after `recvmmsg()` minus the `SO_TIMESTAMPNS` cmsg timestamp from the same datagram. Same clock domain, so no domain-conversion drift. This isolates the kernel socket RX/wakeup path directly (NAPI scheduling, sk_buff processing, cross-core wakeup, socket queue dwell) without the percentile-distribution confound of subtracting two unrelated histograms. The ~16 µs result is large compared to typical commodity Linux figures (~2-5 µs), consistent with NIC driver coalescing/wakeup characteristics on this T2 MacBook test rig.

\*\* **Queue Wait** is a representative single-run value, more sensitive to Strategy hot-loop scheduling (drain cadence, `epoll_wait(0)` syscall, order-send blocking) than the order-send path. See Interpretation below for run-to-run stability.

\*\*\* **TSC inner span** is the previous-generation TSC-only metric: post-`recvmmsg()` userspace return → post-`send()` return. Narrower window than Tick-to-Trade — excludes the ~16 µs RX kernel path. Kept as a TSC-domain cross-validation against the kernel-timestamp-based Tick-to-Trade.

Order RTT p999 is reported from the current run; raw max can extend far beyond the histogram range due to timeout/cancel lifecycle outliers and is not representative of median-path network RTT.

#### Interpretation

- **Tick-to-Trade p50 components are consistent with the kernel-bound interpretation.** Components are measured on different sample populations (RX kernel path is all-tick paired delta; Tick-to-Trade and Order Send path are sent-order only), so their p50s are not strictly additive — but the magnitudes line up:

  ```
  RX kernel path  (t_userspace_rx_ns - t_kernel_rx) : 16.1 µs  (all-tick paired delta)
  Userspace work  (Feed + Queue Wait + Strategy + per-item) :  ~1 µs
  Order Send path (risk + serialize + send + check) : 11.3 µs  (PROFILE_STAGES, sent-orders)
                                                      ───────
  Approximate sum                                    : ~28.4 µs
  Tick-to-Trade measured                             :  33.0 µs
  ```

  The ~4.5 µs residual is not isolated by these stages and likely includes per-item rdtsc instrumentation, two `clock_gettime(CLOCK_REALTIME)` calls (vDSO), and strategy-thread overhead not attributed to any single PROFILE_STAGES stage — for example `epoll_wait(0)` per loop iteration, drain-batch indexing, and the timestamp-store on the dequeue path. The kernel socket stack on RX (~16 µs) and the order-send path that depends on the kernel TCP stack (~11 µs) together account for the bulk of Tick-to-Trade. Note that **Order Send path is not a pure TX kernel measurement** — it also includes risk check, payload serialization, message framing, and post-send result/log handling executed before the timestamp; a strictly TX-kernel-only stage would require dedicated rdtsc points immediately before and after `send_nonblocking()`.

- **Production HFT comparison.** Published wire-to-wire numbers for kernel-bypass HFT systems (e.g., Solarflare OpenOnload + checksum-offload NIC) can reach sub-2 µs. The userspace trading logic in this system already operates at ~1 µs — competitive with that range. The remaining ~32 µs gap is the kernel socket stack on both the RX side (~16 µs) and the order-send path that depends on the kernel TCP stack (~11 µs, though that figure includes risk + serialize cost), not the trading logic. Kernel bypass with a compatible NIC is the canonical mitigation.

- **Feed Parse and Strategy are pure CPU work** (~100-300 ns) — parsing a fixed 36-byte datagram and evaluating a spread condition. Independent of network I/O by construction (no syscalls, no socket operations on these measured paths).

- **RX kernel path is unusually large on this rig (~16 µs).** Commodity Linux + GbE typically sits at 2-5 µs. The 16 µs measurement is consistent with NIC driver coalescing / wakeup-scheduling characteristics of the T2 MacBook running Linux — this is a laptop, not a server NIC. The paired-delta validation (`clock_gettime(CLOCK_REALTIME)` immediately after `recvmmsg()` minus the same-domain `SO_TIMESTAMPNS` cmsg timestamp) confirms the path is real, not a percentile-distribution artifact.

- **Queue Wait** starts at `t0` (post-recv, before parse) and ends at `t_drain` (post-batch-drain on the Strategy thread):

  ```
  MD Thread:
    recvmmsg()
    t0 = rdtsc()            ← Queue Wait starts here
    parse() + spsc.push()

  Strategy Thread:
    drain(batch, 64)        ← dequeue up to 64 items
    t_drain = rdtsc()       ← Queue Wait ends here
    process each item
  ```

  **Queue Wait** (`t_drain - t0`) is how long data sat in transit — parse, SPSC push, cross-thread scheduling delay, and Strategy loop overhead until `drain()` is called. All items in the same batch share the same `t_drain`, so batch-internal processing time is excluded. This is not pure SPSC residence time — the component benchmark (~5 ns push+pop) measures that in isolation.

  **Why Queue Wait p99 is large** (~20 µs on two-machine): Queue Wait reflects how long it takes the Strategy thread to return to `drain()`. When the previous loop iteration involves order sends, TCP response processing, or timeout handling, the next `drain()` is delayed — and items pushed by the MD thread during that time accumulate longer Queue Wait values:

  ```
  MD thread:   push(A)  push(B)  push(C)  push(D)  push(E)
                 │        │        │        │        │
  Strategy:  ─── previous loop busy (order send + TCP response) ───
                                                     drain()
                                                     t_drain

  Queue Wait[A] = t_drain - t0[A] = large (pushed 25 µs ago)
  Queue Wait[E] = t_drain - t0[E] = small (pushed just now)
  ```

  The Strategy thread's loop overhead (TCP `send()`, `epoll_wait(0)` syscalls per iteration, heartbeat/timeout checks) contributes to the drain cadence. Note: Queue Wait measures the TSC `t0` (post-`recvmmsg()` userspace return) to `t_drain` (post-batch-drain on the Strategy thread) span — it does not include the RX kernel path that the headline Tick-to-Trade now captures.

  Queue Wait is a representative single-run value. The p50 value is small (~1 µs); the more interesting quantity is the p99 tail, which reflects Strategy-loop scheduling jitter under order-send pressure. Treat Queue Wait as a representative diagnostic rather than a stable benchmark until more runs are collected.

#### Caveats

- **Sample populations differ across metrics.** Queue Wait and RX kernel path are recorded on **every** market data update (all ticks). Tick-to-Trade is recorded **only** when an order or modify is actually sent. At 100 µs tick interval (~10k ticks/sec) with ~100 orders/sec, all-ticks metrics have ~100× more samples than Tick-to-Trade, and most of those samples are from "no order sent" loops which are faster.

- **What's still missing from wire-to-wire.** Tick-to-Trade as measured here is **kernel-software-timestamped**, not hardware wire-to-wire. On the RX side, the `SO_TIMESTAMPNS` cmsg is attached by the kernel during kernel receive processing — the NIC-hardware-ingress-to-kernel-SW-timestamp interval is *not* captured. On the TX side, `t4`/`post_send_ns` is recorded immediately after `send_nonblocking()` returns, which corresponds to socket/TCP send-path enqueue completion (not NIC dispatch or PHY transmission completion); any network propagation/switching delay is unmeasured. True hardware wire-to-wire would require RX/TX HW timestamping NICs (e.g., Solarflare X2522, Mellanox ConnectX), which this rig does not have.

- **Tick interval changes Queue Wait sample composition.** Queue Wait is measured on the TSC side (post-`recvmmsg()` userspace → `t_drain`), so its semantics depend on how packets arrive in userspace. With a dense feed (100 µs), packets accumulate in the kernel socket buffer before `recvmmsg()` drains them in a batch; the kernel queuing time is invisible to Queue Wait (but **is** visible to the new Tick-to-Trade headline, since that starts from the kernel SW RX timestamp). With a sparse feed (1000 µs), packets rarely queue in the kernel, so Queue Wait dominates instead.

#### Bottlenecks and improvements

- **Tick-to-Trade is dominated by kernel network stack on both sides**. RX kernel path is ~16.1 µs (all-tick paired delta); Order Send path is ~11.3 µs (sent-order, includes risk check + serialize + `send_nonblocking()`). The pure kernel TX stack inside `send_nonblocking()` likely accounts for much of the Order Send path but is not directly isolated — risk and serialize add some overhead. Together they are the dominant cost; userspace trading logic itself (Feed Parse + Queue Wait + Strategy + per-item overhead) is ~1 µs, competitive with kernel-bypass HFT systems' userspace processing time.
- **Largest improvement: kernel bypass on RX side**. OpenOnload (Solarflare) or VMA (Mellanox) via `LD_PRELOAD` replaces the kernel network stack with a userspace implementation. Socket API (`send`, `recv`, `epoll`) remains identical — no code changes required. This is designed to reduce the ~16 µs RX kernel path and the kernel TX cost inside `send_nonblocking()`, and Tick-to-Trade could move toward the userspace-plus-risk-plus-serialize floor (a few µs) plus the unmeasured NIC PHY time. Requires a compatible NIC (Solarflare X2522, Mellanox ConnectX) which this T2 MacBook test rig does not have.
- **RX kernel path tail (p999 ~88 µs)** indicates occasional large delays in NAPI scheduling or wakeup delivery, likely from background interrupts on the non-isolated core that handles NIC IRQs. Manual NIC IRQ affinity to a known non-isolated core (currently irqbalance is off but IRQ affinity is not pinned) is the next investigation step.
- **TSC inner span p999 ~55 µs** for the narrower userspace-boundary metric is consistent with occasional Strategy-loop scheduling jitter under order-send pressure (TCP response handling, timeout cancellation). `idle=poll` would likely reduce these spikes but was not used in this test due to thermal constraints of the test hardware.
