#include "ObjectPool.h"
#include "LimitOrder.h"
#include "OrderBook.h"
#include "OrderFactory.h"
#include "Engine.h"
#include "IJournal.h"
#include "JournalEvent.h"
#include <iostream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <random>
#include <iomanip>
#include <memory>
#include <functional>

using Clock = std::chrono::steady_clock;

// Run `fn` `trials` times and return the FASTEST wall time in ms. Taking the
// minimum is the standard way to microbenchmark on a busy/shared machine: the
// OS can only ever make a run SLOWER (scheduling, page faults, other tenants),
// so the fastest observed run is the closest estimate of the true cost. This
// matters a lot in this sandbox, which shares CPU with other work.
double bestOfMs(int trials, const std::function<void()>& fn) {
    double best = 1e300;
    for (int t = 0; t < trials; ++t) {
        auto s = Clock::now();
        fn();
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - s).count();
        best = std::min(best, ms);
    }
    return best;
}

// A single place to stash the headline numbers so we can reprint them together
// at the end as a clean, copy-pasteable block (handy when turning a run into
// resume bullets). Filled in as each benchmark section runs.
struct ResumeNumbers {
    double poolSpeedup = 0;
    // Allocation latency distribution (ns per op), heap vs pool - backs the
    // "pool has tighter, more predictable latency" claim with actual tail numbers.
    double heapAllocP50 = 0, heapAllocP99 = 0, heapAllocMax = 0;
    double poolAllocP50 = 0, poolAllocP99 = 0, poolAllocMax = 0;
    double p50 = 0, p95 = 0, p99 = 0, p999 = 0, maxUs = 0;
    double matchThroughput = 0;      // orders/sec, pure OrderBook matching
    double engineThroughput = 0;     // orders/sec, full engine (queue + matching + WAL)
    double walNsPerEvent = 0;        // cost of one WAL append (build event + store)
    double walEventsPerSec = 0;      // => how far journaling is from being a bottleneck
    double recoveryEventsPerSec = 0; // replay speed
    double recoveryMs = 0;           // time to replay the whole log
    long   recoveredEvents = 0;
};

// ---------------------------------------------------------------------
// PART 1: heap allocation vs pool allocation, for the SAME work
// (construct then destroy N LimitOrders). Two things are measured:
//   (a) total-time speedup (best-of-5), and
//   (b) the per-operation latency DISTRIBUTION (p50/p99/max), because the real
//       reason low-latency systems pool memory isn't raw speed - it's PREDICTABILITY.
//       A general allocator is usually fast but occasionally takes a slow path
//       (free-list search, coalescing, asking the OS for more memory), which shows
//       up as a fat tail. A pool hands out a preallocated slot (pointer bump), so
//       the pure allocator cost is both lower and steadier. We measure per-batch to
//       stay well above clock granularity, then report the distribution so the claim
//       is DATA, not assertion. Reality check on shared hardware: the p50 (pure
//       allocator cost, no preemption) reliably favours the pool, but the p99/max
//       tail of a tens-of-ns op on a busy VM is dominated by OS scheduling and flips
//       run to run - so we read p50 as the allocator signal here and are explicit
//       that a clean tail measurement needs a quiet, core-pinned machine.
// ---------------------------------------------------------------------
namespace {
// p in [0,1] over an already-sorted vector.
double pctl(const std::vector<double>& sorted, double p) {
    return sorted[static_cast<std::size_t>(p * (sorted.size() - 1))];
}
} // namespace

void allocationBenchmark(ResumeNumbers& out) {
    std::cout << "=== Part 1: heap allocation vs object pool ===\n";
    const int N = 500000;

    double heapMs = bestOfMs(5, [&] {
        for (int i = 0; i < N; ++i) {
            LimitOrder* o = new LimitOrder(i, 1, Side::BUY, 10, 100.0);
            delete o;
        }
    });

    ObjectPool<LimitOrder> pool(1024); // small pool is fine - we release
                                        // each object before acquiring the next
    double poolMs = bestOfMs(5, [&] {
        for (int i = 0; i < N; ++i) {
            LimitOrder* o = pool.acquire(i, 1, Side::BUY, 10, 100.0);
            pool.release(o);
        }
    });

    out.poolSpeedup = heapMs / poolMs;
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "Heap new/delete : " << N << " objects in " << heapMs << " ms\n";
    std::cout << "Object pool     : " << N << " objects in " << poolMs << " ms\n";
    std::cout << std::setprecision(2) << "Speedup: " << out.poolSpeedup << "x\n";

    // ---- (b) latency distribution ----
    // Time in batches of B ops: a single new/delete is ~tens of ns, right at the
    // clock's resolution, so we divide a batch time by B to get a stable ns/op
    // sample. A rare slow allocation still stands out as a high-batch outlier.
    const int B = 100;
    const int batches = N / B;
    std::vector<double> heapNs, poolNs;
    heapNs.reserve(batches);
    poolNs.reserve(batches);

    for (int b = 0; b < batches; ++b) {
        auto s = Clock::now();
        for (int i = 0; i < B; ++i) { LimitOrder* o = new LimitOrder(i, 1, Side::BUY, 10, 100.0); delete o; }
        heapNs.push_back(std::chrono::duration<double, std::nano>(Clock::now() - s).count() / B);
    }
    for (int b = 0; b < batches; ++b) {
        auto s = Clock::now();
        for (int i = 0; i < B; ++i) { LimitOrder* o = pool.acquire(i, 1, Side::BUY, 10, 100.0); pool.release(o); }
        poolNs.push_back(std::chrono::duration<double, std::nano>(Clock::now() - s).count() / B);
    }
    std::sort(heapNs.begin(), heapNs.end());
    std::sort(poolNs.begin(), poolNs.end());
    out.heapAllocP50 = pctl(heapNs, 0.50); out.heapAllocP99 = pctl(heapNs, 0.99); out.heapAllocMax = heapNs.back();
    out.poolAllocP50 = pctl(poolNs, 0.50); out.poolAllocP99 = pctl(poolNs, 0.99); out.poolAllocMax = poolNs.back();

    std::cout << std::setprecision(1);
    std::cout << "Per-op latency (ns), heap vs pool:\n";
    std::cout << "  p50: " << out.heapAllocP50 << " vs " << out.poolAllocP50
              << "   (pool " << std::setprecision(2) << (out.heapAllocP50 / out.poolAllocP50)
              << "x faster at the median)\n" << std::setprecision(1);
    std::cout << "  p99: " << out.heapAllocP99 << " vs " << out.poolAllocP99 << "\n";
    std::cout << "  max: " << out.heapAllocMax << " vs " << out.poolAllocMax << "\n";
    std::cout << "  NOTE: p50 is pure allocator cost, and the pool reliably wins it.\n"
                 "  The p99/max tail of a tens-of-ns op on a SHARED VM is dominated by OS\n"
                 "  preemption, not the allocator, so the tail flips run to run. Read p50\n"
                 "  as the allocator signal here; the tail wants a quiet, core-pinned box.\n\n";
}

// ---------------------------------------------------------------------
// PART 2: latency percentiles for order-matching, not just an average.
// AVERAGES HIDE TAIL LATENCY - a system that's fast 99% of the time and
// catastrophically slow 1% of the time can have the same average as one
// that's uniformly mediocre. Real systems (and the "Introduction to Systems
// Performance and Measurement" material) care about p50/p95/p99/p99.9
// precisely because tail latency is what users actually notice.
// ---------------------------------------------------------------------
void latencyBenchmark(ResumeNumbers& out) {
    std::cout << "=== Part 2: order-matching latency percentiles + throughput ===\n";
    OrderBook book;
    const int N = 100000;
    std::vector<double> latenciesUs; // one measurement per order, in microseconds
    latenciesUs.reserve(N);

    std::mt19937 rng(42);
    std::uniform_real_distribution<double> priceDist(95.0, 105.0);
    std::uniform_int_distribution<int> qtyDist(1, 50);
    std::bernoulli_distribution sideCoin(0.5);

    auto wallStart = Clock::now();
    for (int i = 0; i < N; ++i) {
        Side side = sideCoin(rng) ? Side::BUY : Side::SELL;
        auto order = OrderFactory::createOrder(
            OrderKind::LIMIT, i, 1, side, qtyDist(rng), priceDist(rng));

        auto t0 = Clock::now();
        book.addOrder(std::move(order));
        auto t1 = Clock::now();

        latenciesUs.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    double wallMs = std::chrono::duration<double, std::milli>(Clock::now() - wallStart).count();

    std::sort(latenciesUs.begin(), latenciesUs.end());
    auto percentile = [&](double p) {
        return latenciesUs[static_cast<std::size_t>(p * (latenciesUs.size() - 1))];
    };

    out.p50 = percentile(0.50);
    out.p95 = percentile(0.95);
    out.p99 = percentile(0.99);
    out.p999 = percentile(0.999);
    out.maxUs = latenciesUs.back();
    out.matchThroughput = N / (wallMs / 1000.0);

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Processed " << N << " orders\n";
    std::cout << "  p50 (median): " << out.p50 << " us\n";
    std::cout << "  p95:          " << out.p95 << " us\n";
    std::cout << "  p99:          " << out.p99 << " us\n";
    std::cout << "  p99.9:        " << out.p999 << " us\n";
    std::cout << "  max:          " << out.maxUs << " us\n";
    std::cout << "  throughput:   " << std::setprecision(0) << out.matchThroughput
              << " orders/sec (pure matching)\n";
    std::cout << std::setprecision(3)
              << "  (p99 vs p50 ratio: " << (out.p99 / out.p50)
              << "x - shows tail latency spread)\n\n";
}

// Runs a fixed workload through the FULL engine (queue -> matching thread) and
// returns orders/sec end-to-end (submit until the matching thread has drained
// and flushed every order). This is the "system" throughput number.
double engineThroughputOnce(int nOrders) {
    Engine engine;
    engine.start();

    std::mt19937 rng(7);
    std::uniform_real_distribution<double> priceDist(98.0, 102.0);
    std::uniform_int_distribution<int> qtyDist(1, 20);
    std::bernoulli_distribution sideCoin(0.5);

    auto start = Clock::now();
    for (int i = 0; i < nOrders; ++i) {
        Side side = sideCoin(rng) ? Side::BUY : Side::SELL;
        engine.submitOrder(OrderFactory::createOrder(
            OrderKind::LIMIT, i, 1, side, qtyDist(rng), priceDist(rng)));
    }
    engine.stop(); // blocks until the matching thread drains EVERY order + flush
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return nOrders / (ms / 1000.0);
}

// ---------------------------------------------------------------------
// PART 3: two separate, HONEST measurements about persistence.
//
// (a) End-to-end engine throughput: orders/sec pushed through the concurrent
//     command queue and matched on the single matching thread. Comparing this
//     to Part 2's pure-matching number shows the cost of the thread-safe
//     handoff (that's what you pay for determinism + safe concurrent submit).
//
// (b) Journal append cost, measured DIRECTLY. Earlier I tried to infer journaling
//     overhead by subtracting two whole-engine runs - but an InMemoryJournal
//     append is just a vector push_back, so that difference was pure scheduling
//     noise (it even came out negative). The honest way is to microbenchmark
//     the append path itself: build a JournalEvent for a command and store it,
//     in a tight loop. This times InMemoryJournal (the enqueue-only contract);
//     the takeaway is that append does NO I/O - it just builds an event and
//     stores it. PostgresJournal::append does the same job plus a mutex + a
//     condition-variable notify (hundreds of ns), but still no disk I/O on this
//     thread - the fsync is paid on a background thread, batched, and measured
//     separately by persistence_demo on a machine with a real database.
// ---------------------------------------------------------------------
void persistenceBenchmark(ResumeNumbers& out) {
    std::cout << "=== Part 3a: full-engine throughput (queue + matching) ===\n";
    const int N = 200000;
    double best = 1e300;
    for (int t = 0; t < 3; ++t) best = std::min(best, 1.0 / engineThroughputOnce(N));
    out.engineThroughput = 1.0 / best;
    std::cout << std::setprecision(0);
    std::cout << "End-to-end engine: " << out.engineThroughput << " orders/sec"
              << " (vs " << out.matchThroughput << " pure matching -> the gap is the"
              << " thread-safe queue handoff)\n\n";

    std::cout << "=== Part 3b: journal append cost (in-memory, enqueue-only path) ===\n";
    const int M = 1000000;
    // One reusable order; makeSubmitEvent just reads its fields, so the loop
    // measures JournalEvent construction + append, not order allocation.
    // NOTE: this times InMemoryJournal::append (a std::vector push_back) - i.e.
    // the enqueue-only *contract* every IJournal promises, NOT PostgresJournal.
    // PostgresJournal::append does the same "just enqueue" job but additionally
    // takes a mutex and notifies its writer thread (order of hundreds of ns);
    // the point both share is that NO disk/network I/O happens on this thread -
    // the fsync/round-trip is batched onto a background thread.
    auto sample = OrderFactory::createOrder(OrderKind::LIMIT, 1, 1, Side::BUY, 10, 100.0);
    double ms = bestOfMs(5, [&] {
        InMemoryJournal journal; // fresh each trial
        for (int i = 0; i < M; ++i) {
            journal.append(makeSubmitEvent(*sample, static_cast<std::uint64_t>(i)));
        }
    });
    out.walNsPerEvent = (ms * 1e6) / M; // ms -> ns, per event
    out.walEventsPerSec = M / (ms / 1000.0);
    std::cout << std::setprecision(1);
    std::cout << "In-memory append: " << out.walNsPerEvent << " ns/event  ("
              << std::setprecision(0) << out.walEventsPerSec << " events/sec)\n";
    std::cout << "=> the append path builds an event and enqueues it; it does NO\n"
                 "   disk/network I/O. Durability cost (the fsync) is paid on a\n"
                 "   background thread and amortized by batching in PostgresJournal.\n\n";
}

// ---------------------------------------------------------------------
// PART 4: CRASH-RECOVERY speed. Build a journal by running a workload, then
// throw the engine away and rebuild a brand-new one purely by replaying the
// log. This is the number that backs "recovers state deterministically from
// the write-ahead log" - and it's a real measurement, not a claim.
// ---------------------------------------------------------------------
void recoveryBenchmark(ResumeNumbers& out) {
    std::cout << "=== Part 4: crash-recovery (deterministic log replay) ===\n";
    const int N = 200000;

    // --- Build the log: run a full workload against an InMemoryJournal. ---
    InMemoryJournal journal;
    {
        Engine engine;
        engine.setJournal(&journal);
        engine.start();
        std::mt19937 rng(11);
        std::uniform_real_distribution<double> priceDist(98.0, 102.0);
        std::uniform_int_distribution<int> qtyDist(1, 20);
        std::bernoulli_distribution sideCoin(0.5);
        for (int i = 0; i < N; ++i) {
            Side side = sideCoin(rng) ? Side::BUY : Side::SELL;
            engine.submitOrder(OrderFactory::createOrder(
                OrderKind::LIMIT, i, 1, side, qtyDist(rng), priceDist(rng)));
        }
        engine.stop();
    }
    out.recoveredEvents = static_cast<long>(journal.size());

    // --- Recover: fresh engine, same log, time the full replay. ---
    Engine recovered;
    recovered.setJournal(&journal);
    auto start = Clock::now();
    recovered.recover();
    out.recoveryMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    out.recoveryEventsPerSec = out.recoveredEvents / (out.recoveryMs / 1000.0);

    std::cout << std::setprecision(3);
    std::cout << "Replayed " << out.recoveredEvents << " journaled events in "
              << out.recoveryMs << " ms\n";
    std::cout << std::setprecision(0);
    std::cout << "Recovery speed: " << out.recoveryEventsPerSec << " events/sec\n";
    std::cout << "Rebuilt trade log size: " << recovered.getTradeLog().size() << " trades\n\n";
}

void printResumeBlock(const ResumeNumbers& r) {
    std::cout << "======================================================\n";
    std::cout << " HEADLINE NUMBERS (this run, this machine)\n";
    std::cout << "======================================================\n";
    std::cout << std::setprecision(2);
    std::cout << " Object pool vs new/delete .... " << r.poolSpeedup << "x faster\n";
    std::cout << std::setprecision(1);
    std::cout << " Alloc p50 ns (heap vs pool) .. " << r.heapAllocP50 << " vs "
              << r.poolAllocP50 << "  (pool faster at the median)\n";
    std::cout << std::setprecision(0);
    std::cout << " Matching throughput .......... " << r.matchThroughput << " orders/sec\n";
    std::cout << std::setprecision(3);
    std::cout << " Matching latency p50/p99/p99.9 " << r.p50 << " / " << r.p99
              << " / " << r.p999 << " us\n";
    std::cout << std::setprecision(0);
    std::cout << " Engine throughput (end-to-end) " << r.engineThroughput << " orders/sec\n";
    std::cout << std::setprecision(1);
    std::cout << " Journal append (enqueue) ..... " << r.walNsPerEvent << " ns/event\n";
    std::cout << std::setprecision(0);
    std::cout << " Crash recovery ............... " << r.recoveredEvents << " events in "
              << std::setprecision(1) << r.recoveryMs << " ms ("
              << std::setprecision(0) << r.recoveryEventsPerSec << " events/sec)\n";
    std::cout << "======================================================\n";
    std::cout << "NOTE: run on your own multi-core machine for headline figures;\n"
                 "these are real measurements but the sandbox HW is constrained.\n";
}

int main() {
    ResumeNumbers r;
    allocationBenchmark(r);
    latencyBenchmark(r);
    persistenceBenchmark(r);
    recoveryBenchmark(r);
    printResumeBlock(r);
    return 0;
}
