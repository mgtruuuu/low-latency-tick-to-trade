# Coding Standard

> Living document. Optimized for: **correctness first**, then **deterministic latency**, **bounded memory**, and **easy-to-review code**.
>
> Rule of thumb: if a reviewer cannot reason about it in 30 seconds, it is probably too clever.

---

## 0. Scope and goals

This standard governs all production code in this project: reusable libraries (`libs/`), applications (`apps/`), tests (`tests/`), and benchmarks (`bench/`).

Target platform:

- **Linux x86-64 only.** Co-location servers are exclusively x86-64 Linux.
- **C++20 strict** (`-std=c++20`, no GNU extensions). Compiler: `clang++`.

Non-goals:

- Being fancy.
- Macro-heavy metaprogramming.
- "Framework" building.
- Cross-platform compatibility.

---

## 1. Core principles (the rules behind all rules)

1) **Determinism**

- Avoid unpredictable latency sources: dynamic allocation, locks with contention, syscalls in the hot path, exceptions, I/O.

2) **Bounded resources**

- Fixed capacity queues/buffers.
- Preallocate upfront; fail fast if capacity is insufficient.

3) **Simplicity over cleverness**

- Prefer the simplest design that meets constraints.
- Avoid "clever" micro-optimizations until profiling says it matters.

4) **Correctness is the real performance feature**

- Bugs are the worst latency.

---

## 2. Build modes and flags

### 2.1 Default (development)

- Keep it clean and debuggable.

Suggested:

- Debug: `-O0 -g`
- Release: `-O3 -DNDEBUG`

### 2.2 Low-latency / production mode

Recommended flags:

- `-O3 -DNDEBUG` (set by CMake Release/RelWithDebInfo presets)
- `-march=native` and `-flto` are optional and not currently enabled in presets — add only after profiling confirms benefit

Exception policy:

- **No exceptions anywhere.** Not in hot paths, not in cold paths.
- Hot paths: return `bool`, raw pointer, or enum status codes. Avoid `std::optional` overhead.
- Cold paths: return error codes or `std::optional`. `std::abort()` on unrecoverable errors.
- `-fno-exceptions -fno-rtti` is optional for pure hot-path libraries (e.g., standalone ring-buffer header-only library). Not recommended for full programs with `main()`.

Rule:

- If your project has `main()`, I/O, or config parsing, do **not** use `-fno-exceptions` (for standard library compatibility).
- The code itself still must not throw or catch.

---

## 3. Formatting (non-negotiable)

- Use **clang-format**.
- **Always use braces** for `if/for/while/do` blocks.

Why: reviewers scan code. Consistent formatting reduces mistakes and review time.

---

## 4. Naming

### 4.1 General

| Scope | Style | Example |
| ----- | ----- | ------- |
| Class / Struct / Enum type | `PascalCase` | `OrderBook`, `Side` |
| Function | `snake_case` | `add_order()`, `best_bid()` |
| Variable | `snake_case` | `write_idx`, `buf_size` |
| Local `const` (non-constexpr) | `snake_case` | `const auto saved = errno` |
| `constexpr` variable | `kCamelCase` | `kCacheLineSize`, `kMaxOrders` |
| Global / namespace constant | `kCamelCase` | `kHugePageSize2MB` |
| Enum constant | `kCamelCase` | `kOk`, `kBid` |
| Namespace | `snake_case` | `mk`, `mk::sys` |

### 4.2 Files

- File names: `snake_case` (`ring_buffer.hpp`, `order_book_test.cpp`).
- Headers: `.hpp`. Source: `.cpp`. Tests: `*_test.cpp`. Benchmarks: `*_bench.cpp`.

### 4.3 Members

- **Class** members (private/protected) end with trailing underscore: `size_`, `head_`, `socket_fd_`.
- **Struct** public members do NOT use trailing underscore: `best_bid`, `fill_price`, `client_order_id`.

### 4.4 Abbreviations

- Prefer clear names over abbreviations.
- Common and acceptable: `fd`, `idx`, `len`, `buf`, `ts`.

---

## 5. C++ subset and "safe defaults"

### 5.1 Headers and namespaces

- Never: `using namespace std;`.
- Include what you use.
- Prefer forward declarations when it reduces build time and avoids include cycles.
- All headers must have `#pragma once`.

### 5.2 Initialization

- Prefer **brace initialization** when it improves safety:
  - `int x{0};`
  - `std::array<int, 8> a{};`
- Use `=` initialization when it reads better for non-narrowing scalars:
  - `int x = 0;`

Rule: choose the style that is **harder to misuse** in that context.

### 5.3 `auto` usage

Use `auto` when the type is **visible in the right-hand side**:

```cpp
// Good — type is obvious from RHS
auto x = static_cast<int>(3.14);
auto p = std::make_unique<Order>();
auto it = map.begin();
const auto bytes = serialize(buf, msg);   // return type known from API
```

Use **explicit types** when the RHS does not reveal the type:

```cpp
// Good — explicit type for literals and ambiguous expressions
int count = 0;
algo::Price price = 1'000'000;
std::uint64_t mask = 0xFF;
```

```cpp
// Bad — type not obvious, forces reader to look up return type or guess
auto count = 0;           // int? long? uint32_t?
auto price = bid + ask;   // could overflow depending on type
```

Rationale: in latency-sensitive code the **exact numeric type** (width, signedness) affects correctness (overflow), performance (register width), and wire compatibility. Explicit types make these properties visible at the declaration site.

Rule: if a reader cannot determine the type within 2 seconds by looking at the line alone, use an explicit type.

### 5.4 Const-correctness

- Use `const` by default for local variables that are not modified after initialization.
- Prefer `const auto` for immutable scalars, views, and computed values.
- Do **not** apply `const` to local variables that will be returned or moved — it prevents NRVO and blocks move semantics.
- Prefer `std::span<const T>` for read-only views.

Note: `misc-const-correctness` in clang-tidy does not check header-only inline functions (known limitation). Rely on coding habit.

### 5.5 `noexcept`

- Add `noexcept` to:
  - Move constructors and move assignment operators (required for STL container optimizations).
  - Public API functions where the no-throw contract is part of the interface.
- Do not blanket-apply to every function. The compiler does not need `noexcept` on private helpers to optimize them.

### 5.6 `[[nodiscard]]`

- Add `[[nodiscard]]` for status-returning APIs.

### 5.7 Branch hints

- Use `[[likely]]` / `[[unlikely]]` for branches with known hot/cold distribution.
- Do not sprinkle everywhere; only where profiling or design makes the bias clear.

### 5.8 Preferred defaults

Vocabulary types:

- `std::array`, `std::span`, `std::optional`.

Language features to prefer by default:

- `constexpr` / `consteval` for compile-time evaluation.
- `concepts` (`requires` clauses) instead of SFINAE or raw templates for constraints.
- `noexcept` on move operations and public API boundaries (see §5.5).

### 5.9 STL usage (when to use vs avoid)

Use STL when:

- It matches the semantics exactly
- Performance is acceptable (profile if unsure)
- Examples:
  - `std::sort`, `std::lower_bound`
  - `std::array`
  - `std::span`
  - `std::optional` (when `T` is cheap to construct)
  - `std::vector` with `reserve()` / pre-sizing when growth is controlled

Avoid STL when:

- It introduces hidden allocations in the hot path
- You need custom memory layout or bounded capacity by design
- Examples:
  - Ring buffer / bounded queues (custom)
  - `std::queue` (often backed by `deque` with allocations)
  - `std::unordered_map` inserts in hot path (allocations + rehash)
  - `std::string` in hot path (heap allocation, SSO is not guaranteed to be enough)
  - `std::function` (type-erased callable, may heap-allocate)

Rule of thumb:

- Use STL unless you can clearly explain why custom is better.
- Re-inventing `std::sort` is wasteful; re-inventing a ring buffer with specific constraints is justified.

### 5.10 Undefined behavior avoidance

UB is silent, unpredictable, and often optimization-dependent. A UB-triggered miscompilation can cause wrong trades.

Rules:

- **Signed integer overflow is UB.** Use unsigned arithmetic for wrapping behavior (e.g., ring buffer indices). If signed is needed, check before operating.
- **Type punning via pointer cast is UB** (strict aliasing violation). Use `std::memcpy` or `std::bit_cast` (C++20) for reinterpreting bytes. Note: `reinterpret_cast` is valid for POSIX API casts (e.g., `sockaddr*` ↔ `sockaddr_in*`) and `std::byte*` conversions — these are not type punning.
- **Uninitialized reads are UB.** Always initialize variables (`{}` or `= 0`).
- **Use-after-move**: a moved-from object is in a valid but unspecified state. Do not read from it without reassigning first.
- **Dangling references**: do not return references to locals. Be careful with `std::span` and `std::string_view` outliving the data they point to.

Example (type punning):

```cpp
// Bad: strict aliasing violation (UB)
auto val = *reinterpret_cast<std::uint32_t*>(buf);

// Good: memcpy (compiler optimizes to a single load)
std::uint32_t val;
std::memcpy(&val, buf, sizeof(val));

// Good: C++20 bit_cast (same size, both trivially copyable)
auto val = std::bit_cast<std::uint32_t>(four_bytes);
```

### 5.11 Move semantics and Rule of 5

If a class defines any of: destructor, copy constructor, copy assignment, move constructor, move assignment — define or `= delete` all five.

Common patterns in this project:

- **Move-only types** (RAII wrappers: `ScopedFd`, `MmapRegion`, queues):
  - Delete copy constructor and copy assignment.
  - Define move constructor and move assignment as `noexcept`.
  - Define destructor.
- **Non-movable types** (intrusive list nodes, objects managed by a pool):
  - Delete both copy and move operations.
- **Value types with no resources** (plain data structs):
  - Use compiler-generated defaults (`= default` or omit entirely).

Rule: if your class owns a resource (fd, pointer, mmap), it must be move-only or non-copyable. Never allow implicit copies of resource-owning types.

---

## 6. Control flow and loops

### 6.1 Braces

Always:

```cpp
if (ok) {
  do_x();
}
```

Reason: prevents classic one-line bugs and makes diffs safer.

### 6.2 Loop style

- Prefer range-for for simple iteration:

```cpp
for (auto& x : v) {
  use(x);
}
```

- Prefer index-based loops when you need indices.
- Countdown loops are allowed when they improve clarity:

```cpp
for (std::size_t i = n; i-- > 0;) {
  // use i
}
```

Rule: optimize for readability first; micro-style choices must not slow comprehension.

---

## 7. Error handling

### 7.1 No exceptions anywhere

- Do not throw or catch exceptions in any code path (hot or cold).
- `std::abort()` on unrecoverable errors.

Recommended patterns:

- Hot path: `bool`, raw pointer, or enum status codes. Avoid `std::optional` overhead.
- Cold path: `std::optional<T>` for "maybe produces a value", or `enum class Status` for richer error reporting.

### 7.2 Two-level assertion pattern

Two levels of runtime checking:

1. **`std::abort()`** — Unrecoverable errors that must be caught even in release (capacity overflow, mmap failure, fatal misconfiguration).
2. **`assert()`** — Precondition and invariant verification with zero release overhead (null checks, bounds checks, counters, `in_use_` trackers).

In practice, most precondition checks use `assert()` for zero release overhead. `std::abort()` is reserved for cases where continuing would corrupt state (e.g., pool exhaustion, failed resource acquisition).

Wrap debug-only variables in `#ifndef NDEBUG` to avoid unused-variable warnings and dead code in release:

```cpp
#ifndef NDEBUG
std::uint32_t in_use_{0};
#endif

void push(std::uint32_t idx) {
  // Precondition check (debug only, zero release overhead)
  assert(idx < capacity_);

  slots_[idx] = value;

#ifndef NDEBUG
  ++in_use_;
  // Invariant check (debug only)
  assert(in_use_ <= capacity_);
#endif
}
```

Rule: no custom assertion macros. Standard `assert()` and `std::abort()` are sufficient.

---

## 8. Memory management

### 8.1 General rules

- Avoid allocations in the hot path.
- Prefer:
  - `std::array`, fixed-size buffers
  - pre-sized `std::vector` (reserve once, then never grow)
  - object pools / free-lists for node-based structures

### 8.2 RAII and ownership

- **All resource ownership must be explicit** (RAII wrappers, `std::unique_ptr` with custom deleters, scoped wrappers).
- Prefer value types.
- Prefer `std::unique_ptr` for ownership when you need dynamic lifetime.
- Avoid `std::shared_ptr`. Ownership is always clear and single-owner in this project.

### 8.3 Banned constructs

Project-wide bans (all code paths):

- Exceptions (`throw` / `try` / `catch`) — see §7.1

Hot-path-only bans:

- `new` / `delete` (dynamic allocation)
- `std::iostream` (`std::cout`, `std::cerr`, etc.)
- Virtual dispatch (`virtual` functions)
- `std::string` (heap allocation)
- `std::function` (potential heap allocation)

### 8.4 Alignment and false sharing

- For contended counters / indices, separate cache lines:
  - `alignas(64)` on frequently written fields.

### 8.5 Data layout (cache and memory efficiency)

Hot structures:

- Pack related hot fields together
- Separate hot fields from cold fields

Example:

```cpp
struct Order {
  // Hot fields (frequently accessed)
  std::uint64_t order_id;
  std::int32_t price;
  std::int32_t quantity;

  // Cold fields (rarely accessed)
  char client_id[16];
  std::uint64_t timestamp_ns;
};
```

Arrays and vectors:

- Prefer **SoA** (Struct of Arrays) for hot loops over many elements
- Prefer **AoS** (Array of Structs) for small sets with mixed access

Example SoA:

```cpp
struct Prices {
  std::array<std::int32_t, 1000> bids;
  std::array<std::int32_t, 1000> asks;
};
```

Padding and alignment:

- Use `alignas(kCacheLineSize)` or `alignas(64)` for contended atomics / indices. This project is x86-64 only (`kCacheLineSize = 64` in `hardware_constants.hpp`), so the literal is always correct. Avoid `std::hardware_destructive_interference_size` — it is a compile-time constant that may not match the runtime environment, and GCC emits `-Winterference-size` warnings.
- Do **not** align small structs blindly (wastes space and can hurt cache)

Example:

```cpp
struct alignas(64) ProducerState {
  std::atomic<std::uint64_t> tail;
};
```

Rule: profile before adding alignment. Wrong alignment is worse than none.

---

## 9. Concurrency rules

### 9.1 Design preference order

1) Single-threaded design (if enough)
2) SPSC queues (best simplicity/perf)
3) MPSC/MPMC bounded queues (only when needed)
4) Locks (only on cold paths, or when contention is low and predictable)

### 9.2 Atomics

Memory ordering rules:

- `memory_order_relaxed`: pure counters (no synchronization)
- `memory_order_acquire/release`: producer-consumer handoff (most common)
- `memory_order_seq_cst`: avoid unless you need global ordering. Requires heavy store buffer flush (e.g., `MFENCE` or locked instruction on x86).

Common patterns:

- SPSC queue: release on publish (writer), acquire on consume (reader)
- MPSC/MPMC queues: often require more careful design (CAS loops, ABA handling, sometimes fences)
- 128-bit CAS (`CMPXCHG16B`): tagged pointer for ABA prevention in lock-free stacks. Requires `alignas(16)` on the double-width struct.
- Counters: relaxed is fine if exact value is not used for synchronization

Pitfalls to avoid:

- Do not "sprinkle atomics" and assume it is safe; define ownership + happens-before first
- A relaxed load in a polling loop may need an acquire load at the point you consume data
- `compare_exchange_*` under contention can be expensive; design to reduce contention

Rule:

- If you cannot explain why relaxed is safe, use acquire/release.

### 9.3 Thread ownership

- Make ownership explicit:
  - Which thread writes what.
  - Which thread reads what.
  - Who closes a socket.

### 9.4 No blocking in hot path

- Avoid waits, sleeps, condition_variable in latency-critical loops.

### 9.5 `volatile` is not for synchronization

- `volatile` prevents compiler reordering of a single variable's accesses, but provides **no atomicity and no CPU memory ordering guarantees**.
- For inter-thread communication, always use `std::atomic` with explicit memory ordering.
- `volatile` has legitimate uses: memory-mapped I/O registers, signal handlers. Not for lock-free data structures.

---

## 10. Networking / I/O (project guidelines)

API preference:

- **Portable baseline**: POSIX sockets + `poll()` (or `select()`)
- **Linux target**: `epoll` (edge-triggered when appropriate)
- **Modern (optional)**: `io_uring` — only when the completion-based model is well understood

Hot-path rules:

- Non-blocking sockets
- Batch reads: read multiple messages per wakeup
- Batch writes: buffer up to a bounded limit, then write
- Avoid per-message allocations

Separation pattern (I/O vs parsing):

```cpp
// I/O thread: read bytes into a bounded buffer (e.g., ring buffer)
// Parser thread: parse messages from the buffer and process

while (true) {
  // I/O side
  const ssize_t n = ::read(fd, buf, sizeof(buf));
  if (n > 0) {
    ring.write(buf, static_cast<std::size_t>(n));
  }

  // Parsing side
  Message msg;
  while (ring.try_read(msg)) {
    process(msg);
  }
}
```

Guideline:

- Start with the simplest model that is correct.
- Prefer `epoll` for this project; `io_uring` is a future consideration.

---

## 11. Observability (without hurting latency)

- Hot path:
  - simple counters (atomics or per-thread counters)
  - timestamps sampled, not per-event logging
- Cold path:
  - structured logs

Latency measurement:

- Prefer sampling (not measuring every event)
- On x86, use serialized TSC reads for accurate benchmarks:
  - `rdtsc_start()` (LFENCE + RDTSC): drains CPU pipeline before reading timestamp. Use at measurement start.
  - `rdtsc_end()` (RDTSCP + LFENCE): waits for measured code to complete. Use at measurement end.
  - Raw `rdtsc()`: ~1ns cost but **not serializing**. Use only for low-overhead sampling, not for accurate intervals.

Example (serialized measurement):

```cpp
const std::uint64_t start = rdtsc_start();
// do_work();
const std::uint64_t end = rdtsc_end();
// histogram.add(end - start);
```

Rule: debugging must not become a hidden latency tax.

---

## 12. API design rules

- Prefer narrow, explicit interfaces.
- Avoid "do everything" classes.
- Separate hot/cold paths:
  - e.g., `try_push()` (hot) vs `push_or_abort()` (cold)

### 12.1 Preconditions and invariants

- Document preconditions in comments.
- Enforce with:
  - `assert()` for preconditions and invariants (zero release overhead)
  - `std::abort()` only for unrecoverable errors (pool exhaustion, failed resource acquisition)

---

## 13. Testing and benchmarking

### 13.1 Tests

- Unit tests must be deterministic.
- Avoid sleeps; use barriers/latches where needed.
- Framework: GoogleTest. Use `TEST_F` with fixtures for shared setup; `TEST` for standalone.
- Use `ASSERT_*` (fatal) when subsequent code depends on the result; `EXPECT_*` (non-fatal) otherwise.
- Death tests: use `EXPECT_DEATH` / `EXPECT_EXIT` for crash/abort scenarios. Suite names must end with `*DeathTest`.

### 13.2 Microbenchmarks

- Benchmark the thing you care about.
- Pin down:
  - input sizes
  - warmup
  - CPU affinity (when relevant)

Rule: if you cannot explain the benchmark methodology, do not trust the result.

---

## 14. Review checklist (use before you commit)

- [ ] No allocations in the hot path (or clearly justified).
- [ ] No exceptions anywhere.
- [ ] No `iostream`, `std::string`, `std::function`, virtual dispatch in hot path.
- [ ] Bounded queues/buffers.
- [ ] No data races (ownership documented).
- [ ] Atomics use correct memory ordering.
- [ ] No UB (signed overflow, strict aliasing, uninitialized reads, dangling refs).
- [ ] Braces everywhere.
- [ ] `[[nodiscard]]` on status-returning functions.
- [ ] Tests cover edge cases.
- [ ] Comments explain invariants, not restate code.
- [ ] No false sharing (adjacent hot fields with different writers).
- [ ] No hidden allocations (string concat, `std::function`, container growth, etc.).
- [ ] Move semantics correct (Rule of 5 if you define any special member).
- [ ] Templates: avoid needless bloat; use explicit instantiation only when you know why.
- [ ] RAII for all resource ownership.
- [ ] Debug-only variables wrapped in `#ifndef NDEBUG`.
- [ ] `#pragma once` on all headers.
- [ ] `auto` only when type is visible in RHS; explicit types for literals (§5.3).
- [ ] `const` by default for non-mutated local variables (§5.4).

---

## 15. Future considerations

- "Hot path vs cold path" folder conventions
- Standard status/result type (portable `expected`-like)
- Fixed-capacity vector (`static_vector`) guideline
