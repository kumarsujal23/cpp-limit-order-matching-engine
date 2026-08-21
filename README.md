# Trading Engine (C++17)

A limit order matching engine built to learn (and demonstrate) core systems
programming concepts: OOP design, STL data-structure selection, multithreaded
concurrency, lock-free programming, SIMD vectorization, and performance
measurement.

## What it does

Implements the core of a stock exchange's matching engine:
- Accepts **market** and **limit** buy/sell orders
- Matches them using **price-time priority** (best price first; FIFO within a price level)
- Supports partial fills and order cancellation
- Processes orders concurrently from multiple threads while matching
  deterministically on a single dedicated thread
- Includes a benchmark suite and unit tests

## Architecture

```
Trader ---submits---> Engine ---queues onto---> ThreadSafeQueue
                          |                            |
                          |                    (matching thread pops)
                          v                            v
                       OrderBook <---------------------+
                       /        \
                  bids_ (map)   asks_ (map)
                  price -> FIFO list of resting orders
```

- `Order` (abstract) → `MarketOrder`, `LimitOrder` — polymorphic order types
- `OrderFactory` — Factory pattern for constructing orders
- `OrderBook` — the actual matching logic and price-level data structures
- `Engine` — thread-safe wrapper: many producer threads, one matching consumer thread
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
| Add limit order (matches k resting orders) | O(k log n) | one match + erase per resting order consumed |
| Cancel order | O(log n) | map lookup by price + O(1) list erase via stored iterator |
| Best bid/ask | O(1) | `map::begin()` |

## Measured performance (this sandbox: 1 CPU core — re-run on your own multi-core machine for realistic numbers)

- **Matching latency** (100k random orders): p50 = 0.32 µs, p95 = 0.75 µs, p99 = 1.8 µs. Max spiked to ~2.7 ms once — a reminder that averages hide tail latency; a few `std::map` allocations or OS scheduling hiccups can spike far past the median.
- **Object pool vs heap allocation** (500k `LimitOrder`s): pool was ~1.3x faster than `new`/`delete`, with far more *consistent* per-call latency — the actual motivation for pooling in low-latency systems.
- **Lock-free SPSC queue vs mutex-based queue** (200k items): lock-free was ~2x faster even on one core; the gap should widen further on multiple cores, since the mutex version forces threads to block/wake via the OS while the lock-free version never blocks.
- **SIMD (AVX2) VWAP calculation** over 20M trades: 1.4x speedup vs a deliberately non-auto-vectorized scalar loop, with identical results.
- Verified with **ThreadSanitizer** (`-fsanitize=thread`): zero data races detected across 2000 concurrently-submitted orders from 4 threads.

Re-run these yourself with `./build/benchmark` and `./build/concurrency_demo` — don't just take the numbers above on faith, they're meant to be reproduced.

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
├── main.cpp                    # Week 1-2: order book demo scenarios
├── include/                    # all headers
├── src/                        # all implementations
├── demos/
│   ├── concurrency_demo.cpp    # Week 3: multithreaded submission + lock-free vs mutex
│   └── simd_demo.cpp           # Week 4: AVX2 VWAP calculation
├── benchmarks/
│   └── benchmark.cpp           # Week 4: allocation + latency percentile benchmarks
└── tests/
    ├── TestFramework.h         # minimal dependency-free test harness
    └── tests.cpp                # Week 5: correctness tests
```

## Building and running

### Prerequisites

- CMake 3.16 or newer
- A C++17 compiler: GCC, Clang, or MSVC
- Git, if you are cloning the project
- An AVX2-capable CPU to run `simd_demo`; the other targets do not require AVX2

The project has no third-party runtime dependencies. CMake builds the shared
engine code once as the `engine_core` static library and links it into the
demo, benchmark, and test executables.

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
./build/benchmark        # object-pool and matching-latency measurements
```

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
- Persistent trade log (CSV/append-only file) with replay-based recovery
- Stop orders / iceberg orders (tests how cleanly the `Order` hierarchy extends)
