# Trading Engine (C++17)

A limit order matching engine built to learn (and demonstrate) core systems
programming concepts: OOP design, STL data-structure selection, multithreaded
concurrency, lock-free programming, SIMD vectorization, performance
measurement, and **durable persistence with crash recovery** (event-sourced
write-ahead log in PostgreSQL, real-time market data in Redis).

> Companion docs: **[ARCHITECTURE.md](ARCHITECTURE.md)** (how the pieces fit and
> the hot-path/off-path split), **[INTERVIEW.md](INTERVIEW.md)** (the questions
> this project is designed to answer), and **[RESUME.md](RESUME.md)** (bullet
> points backed by the measured numbers).

## What it does

Implements the core of a stock exchange's matching engine:
- Accepts **market** and **limit** buy/sell orders
- Matches them using **price-time priority** (best price first; FIFO within a price level)
- Supports partial fills and order cancellation
- Processes orders concurrently from multiple threads while matching
  deterministically on a single dedicated thread
- **Journals every command to a write-ahead log and rebuilds its exact state
  after a crash by replaying it** (event sourcing) — durable log in PostgreSQL,
  live trade/quote feed in Redis, both behind interfaces so the core needs
  neither to run or be tested
- Includes a benchmark suite and unit tests (18 tests, including crash-recovery)

## Architecture

```
                      many trader threads
                              |
                    submitOrder / cancelOrder
                              v
                       ThreadSafeQueue         (the ONE ordered command stream)
                              |
                   (single matching thread pops)
                              v
   IJournal  <--- 1. append(event)  ---  Engine  --- 3. publish --->  IMarketDataSink
  (write-ahead                            |                            (real-time feed)
   log, durable)                 2. apply to book                          |
      |                                   v                                v
      v                                OrderBook                        Redis
   Postgres                           /        \                 (PUB/SUB md:trades,
 (events + trades)              bids_ (map)   asks_ (map)          cache md:best_bid...)
      |                         price -> FIFO list of resting orders
      v
   recover(): replay every event in seq order -> exact same book + trades
```

Every command is (1) written to the write-ahead log, (2) applied to the book,
(3) published to the market-data feed — in that order, on one thread. Because
that order is fixed and journaled, replaying the log after a crash reproduces
the book byte-for-byte. See **[ARCHITECTURE.md](ARCHITECTURE.md)** for the full
hot-path / off-path breakdown.

- `Order` (abstract) → `MarketOrder`, `LimitOrder` — polymorphic order types
- `OrderFactory` — Factory pattern for constructing orders
- `OrderBook` — the actual matching logic and price-level data structures
- `Engine` — thread-safe wrapper: many producer threads, one matching consumer thread
- `IJournal` — write-ahead-log interface; `PostgresJournal` (durable), plus
  `InMemoryJournal` / `NullJournal` for tests and persistence-off runs
- `IMarketDataSink` — real-time output interface; `RedisMarketData` (pub/sub +
  cache), plus `NullMarketDataSink`
- `ThreadSafeQueue` — mutex + condition_variable based blocking queue
- `LockFreeSPSCQueue` — atomics-based lock-free alternative, benchmarked against the above
- `ObjectPool` — manual memory pool allocator, benchmarked against heap `new`/`delete`

### Why both mutex and lock-free queues?

They are used for different requirements, not as interchangeable implementations
of the same engine path. `Engine` accepts orders from many trader threads, so its
input queue needs multi-producer safety. The mutex-based `ThreadSafeQueue` is the
simple, correct MPMC-capable choice here: producers block briefly while updating
the queue, and the matching thread sleeps efficiently when there is no work.

`LockFreeSPSCQueue` supports exactly one producer and one consumer. It is therefore
not a drop-in replacement for the engine's input queue. The concurrency demo uses
it in a separate benchmark with one producer and one consumer, where it compares
queue handoff throughput and timing against the mutex queue. It does **not** compare
matching results, and it does not prove that lock-free is always faster in every
workload. The lock-free producer may busy-spin when the ring buffer is full, which
can reduce latency but consume more CPU.

Trading systems often use lock-free or wait-free structures in carefully selected
hot paths, but mutexes are not automatically incorrect. The right choice depends
on producer/consumer cardinality, boundedness, latency targets, CPU budget,
back-pressure, and implementation risk. This project demonstrates both choices
and explains why the engine uses the mutex queue while the SPSC benchmark isolates
the lock-free technique.

## Persistence & crash recovery (event sourcing)

The engine treats the ordered stream of **input commands** as the source of
truth. Every `SUBMIT`/`CANCEL` gets a monotonically increasing sequence number
and is appended to a **write-ahead log** *before* it touches the book. Recovery
is then just: read the log back in `seq` order and re-apply each command.
Because matching is deterministic and single-threaded, replaying the same
inputs reproduces every trade and the exact resting book — so we never need to
persist the trades themselves for correctness.

- **PostgreSQL is the durable system of record** (`PostgresJournal`, via
  libpqxx). Writes go through a background thread that **batches many commands
  into one transaction**, so the expensive `fsync` on commit is amortised over
  hundreds of rows — the same "group commit" idea real databases use. Trades are
  *also* written to a separate `trades` table as an output projection, purely so
  you can run SQL analytics (volume, VWAP, notional).
- **Redis is the real-time market-data path** (`RedisMarketData`, via hiredis):
  it `PUBLISH`es each trade to subscribers and caches the current best bid/ask so
  a late-joining client can `GET` the top of book in O(1). Commands are
  **pipelined** to amortise the network round-trip.
- **Both are optional and hidden behind interfaces** (`IJournal`,
  `IMarketDataSink`). With no journal set, the engine runs fully in-memory — which
  is exactly how the unit tests exercise the recovery *logic* using an
  `InMemoryJournal` test double, no database required.

Two datastores on purpose: Postgres answers *"never lose this"* (durability),
Redis answers *"everyone needs this right now"* (low-latency fan-out). Conflating
them would mean either a slow real-time feed or a non-durable log.

Why the WAL is off the hot path: `append()`/`onTrade()` only enqueue and return;
the matching thread never blocks on disk or network I/O — that work happens on
background threads. A microbenchmark of the in-memory append (the enqueue-only
contract every `IJournal` promises) is ~**46 ns/event**; the real
`PostgresJournal::append` does the same job plus a mutex lock and a
condition-variable notify (order of hundreds of ns), but still no disk I/O on the
matching thread. The actual durability cost is the `fsync`, and that is paid on
the writer thread and amortized by batching — which is the whole point of doing
it off the hot path.

See `demos/persistence_demo.cpp` for the full trade → crash → recover → verify
story against real Postgres + Redis, and **[ARCHITECTURE.md](ARCHITECTURE.md)**
for the design rationale.

## Design decisions (and what I'd say about them in an interview)

| Decision | Why |
|---|---|
| `std::map<double, std::list<Order*>>` for each side of the book | Price levels must stay **sorted** (best price = `begin()`), so `std::map` (red-black tree, O(log n)) beats `unordered_map`. Within a price level, `std::list` gives O(1) erase-by-iterator and doesn't invalidate other iterators when one order is removed — needed for O(log n) cancellation via an index of stored iterators. |
| `unique_ptr<Order>` ownership, raw `Order*` in the book/index | One clear owner (`OrderBook`'s `unordered_map`) avoids reference-counting overhead of `shared_ptr`; the raw pointers handed elsewhere stay valid because `unordered_map` guarantees erasing one entry never invalidates references to others. |
| Single-threaded matching, multi-threaded ingestion | Determinism: matching outcomes must not depend on OS thread-scheduling luck. Parallelism instead belongs at the "many independent order books, one per symbol" level — not inside one book's matching loop. |
| Trades execute at the **resting** order's price | Standard exchange convention — price improvement goes to whoever was already waiting, not the aggressor. |
| Market orders never rest in the book | An unfilled market order is dropped, not queued — queuing would defeat the "fill me now" semantics. |

## Complexity

| Operation | Complexity | Why |
|---|---|---|
| Add limit order (no match) | O(log n) | one `std::map` insert (n = number of distinct price levels) |
| Add limit order (matches k resting orders) | O(k + L·log n) | each of the k fills is O(1) (list `pop_front` + two hash-map erases); the log n is charged only when a price level empties and is removed from the map — once per level L crossed (L ≤ k), so the worst case is O(k log n) |
| Cancel order | O(log n) | map lookup by price + O(1) list erase via stored iterator |
| Best bid/ask | O(1) | `map::begin()` |

## Measured performance

All figures below are **real measurements** from `./benchmark`, taken in a
constrained, shared build sandbox (few cores, contended CPU). They are
reproducible but conservative — **re-run on your own multi-core machine for
headline numbers**, they will typically improve.

- **Matching latency** (100k random orders): p50 ≈ 0.21 µs, p95 ≈ 0.6 µs,
  p99 ≈ 1.3 µs, p99.9 ≈ 4–9 µs (the tail itself moves run to run), and the
  single worst case has spiked to ~2–3 ms. That spread is the point — averages
  hide tail latency; a `std::map` node allocation or an OS scheduling hiccup can
  spike one operation far past the median, which is exactly why we report
  percentiles instead of a mean.
- **Matching throughput**: ~2.4M orders/sec through the order book itself;
  ~1.7M orders/sec end-to-end through the full engine (concurrent command queue
  + matching thread). The gap between the two *is* the price of the thread-safe
  handoff that buys determinism and safe concurrent submission.
- **Journal append (enqueue path)**: ~46 ns/event for the in-memory journal —
  the enqueue-only contract does no I/O. `PostgresJournal::append` adds a
  mutex + notify on top, but the disk `fsync` is deliberately off this thread.
- **Crash recovery**: replays 200,000 journaled commands and rebuilds the full
  book + trade history in ~85 ms (~2.3M events/sec).
- **Object pool vs heap allocation** (500k `LimitOrder`s): pool was ~1.2x faster
  than `new`/`delete` in total time and cut median per-op latency to ~26 ns vs
  ~37 ns (p50). Pointer-bump allocation avoids the general allocator's slow paths;
  that's the motivation for pooling in low-latency systems. (The p99/max tail on a
  shared VM is scheduling-dominated and flips run to run, so the benchmark reports
  p50 as the allocator signal — see `RESULTS.md`.)
- **Lock-free SPSC queue vs mutex-based queue** (200k items): the lock-free
  handoff measured several times faster — roughly 3–7x across runs (mutex
  ≈9M items/sec, lock-free ≈30–65M items/sec), the ratio depending heavily on
  cores and scheduling. This benchmarks queue transport only, not matching; the
  lock-free version never blocks, while the mutex version parks/wakes threads
  through the OS. It is single-producer/single-consumer only, which is why the
  engine's multi-producer input queue still uses the mutex version.
- **SIMD (AVX2) VWAP calculation** over 20M trades: ~1.5x speedup vs a
  deliberately non-auto-vectorized scalar loop, results agreeing to within a
  1e-6 tolerance (the FMA rounds once where the scalar multiply-then-add rounds
  twice, so they are not bit-identical). It is memory-bandwidth bound, which is
  why it is ~1.5x and not the theoretical 4x.
- Verified with **ThreadSanitizer** (`-fsanitize=thread`): zero data races
  across 2000 concurrently-submitted orders from 4 threads.

Re-run these yourself with `./benchmark`, `./concurrency_demo`, and `./simd_demo`
— they are meant to be reproduced, not taken on faith. The Postgres/Redis
durable-throughput numbers come from `./persistence_demo` (needs the databases
up); they depend on your disk, so measure them on your own machine.

## How coursework concepts map onto this project

("Introduction to Systems Performance and Measurement, Garbage Collection,
JIT Compilation, SIMD Optimizations, Multicore Programming, Lock-Free
Programming, Concurrency and Lightweight Threading")

- **Systems performance & measurement** → `benchmarks/benchmark.cpp` measures p50/p95/p99 latency, not just averages, and compares allocation strategies with real timing data.
- **Concurrency & lightweight threading** → `Engine` + `ThreadSafeQueue` implement the classic multi-producer/single-consumer pattern using `std::thread`, `std::mutex`, `std::condition_variable`.
- **Multicore programming** → the concurrency demo runs genuinely parallel trader threads; the design note on *why matching itself stays single-threaded* (determinism over raw parallelism) is a direct, deliberate application of this topic, not an oversight.
- **Lock-free programming** → `LockFreeSPSCQueue` uses `std::atomic` with explicit `memory_order_acquire`/`release` instead of a mutex. The demo compares queue handoff performance, not trade outcomes, and includes an honest discussion of why it is SPSC-only (general lock-free MPMC queues have real, hard problems like the ABA problem).
- **SIMD optimizations** → `demos/simd_demo.cpp` hand-vectorizes a VWAP calculation with AVX2 intrinsics (`_mm256_fmadd_pd`), benchmarked against a forced-scalar baseline.
- **Garbage collection** → C++ has none; `ObjectPool` is a deliberate manual alternative, and the README/code comments draw the direct parallel to why GC pause unpredictability is a real problem in low-latency systems (and why C++ is often chosen specifically to avoid it).
- **JIT compilation** → C++ is ahead-of-time compiled — no runtime warm-up. Worth noting as a genuine trade-off in an interview: AOT compilation means consistent latency from the very first request, at the cost of the runtime profile-guided optimizations a JIT (JVM/V8) can apply *after* observing real workload behavior. Neither is strictly "better" — it's a latency-consistency vs peak-throughput trade-off, and low-latency trading systems consistently favor the former.

## Project layout

```
trading-engine/
├── CMakeLists.txt
├── docker-compose.yml         # Postgres + Redis for the persistence demo
├── main.cpp                   # order book demo scenarios
├── include/                   # all headers (Engine, OrderBook, IJournal, IMarketDataSink, ...)
├── src/                       # core implementations
├── persistence/               # OPTIONAL DB adapters (built only if libpqxx + hiredis found)
│   ├── PostgresJournal.{h,cpp} # durable write-ahead log (libpqxx)
│   └── RedisMarketData.{h,cpp} # real-time trade/quote feed (hiredis)
├── db/
│   └── schema.sql             # events + trades tables (mirrors what the adapter creates)
├── demos/
│   ├── concurrency_demo.cpp   # multithreaded submission + lock-free vs mutex
│   ├── simd_demo.cpp          # AVX2 VWAP calculation
│   └── persistence_demo.cpp   # trade -> crash -> recover -> verify (needs Postgres + Redis)
├── benchmarks/
│   └── benchmark.cpp          # allocation, latency percentiles, throughput, WAL, recovery
├── tests/
│   ├── TestFramework.h        # minimal dependency-free test harness
│   └── tests.cpp              # correctness + crash-recovery tests
├── ARCHITECTURE.md            # design deep-dive (hot path vs off path)
├── INTERVIEW.md               # Q&A this project is built to answer
└── RESUME.md                  # resume bullets backed by the measured numbers
```

## Building and running

### Prerequisites

- CMake 3.16 or newer
- A C++17 compiler: GCC, Clang, or MSVC
- Git, if you are cloning the project
- An AVX2-capable CPU to run `simd_demo`; the other targets do not require AVX2

The core project has **no third-party runtime dependencies**. CMake builds the
shared engine code once as the `engine_core` static library and links it into
the demo, benchmark, and test executables.

The **persistence layer is optional** and only builds if its libraries are
present (`libpqxx` for PostgreSQL, `hiredis` for Redis). If they aren't
installed, CMake prints `Persistence targets DISABLED` and everything else still
builds and tests normally — that separation is the whole point of the `IJournal`
/ `IMarketDataSink` interfaces. To enable it on Ubuntu/WSL:

```bash
sudo apt install -y libpqxx-dev libhiredis-dev
```

### Linux

From the project root, run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run the complete verification flow:

```bash
./build/tests
ctest --test-dir build --output-on-failure
```

Run the demonstrations and benchmarks:

```bash
./build/trading_engine   # order-book scenarios and generated trades
./build/concurrency_demo # concurrent submission and queue comparison
./build/simd_demo        # scalar versus AVX2 VWAP calculation
./build/benchmark        # allocation, latency, throughput, WAL, recovery
```

### Persistence demo (PostgreSQL + Redis)

This is the end-to-end durability + crash-recovery story. Bring up the two
datastores with Docker, install the client libraries, rebuild so CMake picks up
the persistence targets, then run the demo:

```bash
docker compose up -d                       # start Postgres + Redis
sudo apt install -y libpqxx-dev libhiredis-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # reconfigure -> enables persistence
cmake --build build -j
./build/persistence_demo                   # trade -> crash -> recover -> verify + throughput
```

Then inspect the durable state directly:

```bash
docker exec -it trading_postgres psql -U trader -d trading \
  -c "SELECT count(*) FROM events;" \
  -c "SELECT sum(quantity*price) AS notional FROM trades;"
docker exec -it trading_redis redis-cli GET md:last_price
docker compose down          # stop; add -v to also wipe stored data
```

Connection settings default to the values in `docker-compose.yml`; override with
the `PG_CONN`, `REDIS_HOST`, and `REDIS_PORT` environment variables if needed.

### Windows with WSL

WSL lets you use the Linux toolchain while keeping the source code on the
Windows filesystem. Open PowerShell or the VS Code integrated terminal at the
project root, then run the same commands through `wsl`:

```powershell
wsl cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
wsl cmake --build build -j
wsl ./build/tests
wsl ctest --test-dir build --output-on-failure
```

To run the other targets from PowerShell:

```powershell
wsl ./build/trading_engine
wsl ./build/concurrency_demo
wsl ./build/simd_demo
wsl ./build/benchmark
```

If WSL is not installed, open PowerShell as Administrator and run:

```powershell
wsl --install
```

Restart Windows when prompted, open the installed Linux distribution, create
your Linux username and password, and then install the build tools inside WSL:

```bash
sudo apt update
sudo apt install -y build-essential cmake
```

Return to the Windows project directory in PowerShell and use the commands
above. If the project is copied into the Linux filesystem instead, use its
Linux path and run the native Linux commands directly inside WSL.

### What to test and where results appear

- `tests`: correctness checks for price-time matching, partial fills, market
  orders, cancellation, shutdown, restart, validation, and duplicate IDs.
- `ctest`: the CMake test summary; use `--output-on-failure` for details.
- `trading_engine`: readable order-book snapshots and trade records.
- `concurrency_demo`: number of submitted orders, trades, and queue timings.
- `simd_demo`: scalar result, SIMD result, speedup, and equality check.
- `benchmark`: allocation timings and p50/p95/p99 matching latency.

All results are printed to the terminal. Build files and executables are
created under `build/`; no output files are required by the current demos.

### Where inputs come from

The current project is a deterministic simulator, not an interactive exchange
server. Inputs are generated in code:

- `main.cpp` uses fixed example orders to demonstrate resting, matching, partial
  fill, market sweep, and cancellation behavior.
- `demos/concurrency_demo.cpp` generates random limit orders using fixed random
  seeds, four simulated trader threads, prices from 98 to 102, and quantities
  from 1 to 20.
- `benchmarks/benchmark.cpp` generates 100,000 random limit orders for latency
  measurement and constructs 500,000 fixed `LimitOrder` objects for allocation
  measurement.
- `demos/simd_demo.cpp` generates 20 million random prices and quantities for
  the VWAP comparison.
- `tests/tests.cpp` creates explicit orders for repeatable correctness checks.

There is currently no command-line, CSV, network, exchange-feed, or database
input. The next production-oriented step would be an event/input layer that
validates external orders before assigning them to a symbol's order book.

### Optional thread-safety check

On Linux or inside WSL, compile the concurrency demo with ThreadSanitizer:

```bash
g++ -std=c++17 -pthread -fsanitize=thread -Iinclude \
  demos/concurrency_demo.cpp src/*.cpp -o concurrency_demo_tsan
./concurrency_demo_tsan
```

ThreadSanitizer reports data races in the terminal. A clean run ends with the
normal concurrency-demo output and no sanitizer warnings. This command creates
`concurrency_demo_tsan` in the project root; it is safe to delete afterwards.

## Possible extensions

- Per-symbol order books running on separate threads/cores (real multicore parallelism)
- A TCP client-server layer so multiple external processes submit orders
- Periodic snapshots so recovery replays only the tail of the log, not the whole history
- Stop orders / iceberg orders (tests how cleanly the `Order` hierarchy extends)
