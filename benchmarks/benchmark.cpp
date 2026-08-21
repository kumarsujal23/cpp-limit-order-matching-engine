#include "ObjectPool.h"
#include "LimitOrder.h"
#include "OrderBook.h"
#include "OrderFactory.h"
#include <iostream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <random>
#include <iomanip>

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------
// PART 1: heap allocation vs pool allocation, for the SAME work
// (construct then destroy N LimitOrders).
// ---------------------------------------------------------------------
void allocationBenchmark() {
    std::cout << "=== Part 1: heap allocation vs object pool ===\n";
    const int N = 500000;

    // --- heap: new + delete, one at a time ---
    auto start = Clock::now();
    for (int i = 0; i < N; ++i) {
        LimitOrder* o = new LimitOrder(i, 1, Side::BUY, 10, 100.0);
        delete o;
    }
    double heapMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();

    // --- pool: acquire + release from pre-reserved memory ---
    ObjectPool<LimitOrder> pool(1024); // small pool is fine - we release
                                        // each object before acquiring the next
    start = Clock::now();
    for (int i = 0; i < N; ++i) {
        LimitOrder* o = pool.acquire(i, 1, Side::BUY, 10, 100.0);
        pool.release(o);
    }
    double poolMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();

    std::cout << "Heap new/delete : " << N << " objects in " << heapMs << " ms\n";
    std::cout << "Object pool     : " << N << " objects in " << poolMs << " ms\n";
    std::cout << "Speedup: " << (heapMs / poolMs) << "x\n\n";
}

// ---------------------------------------------------------------------
// PART 2: latency percentiles for order-matching, not just an average.
// AVERAGES HIDE TAIL LATENCY - a system that's fast 99% of the time and
// catastrophically slow 1% of the time can have the same average as one
// that's uniformly mediocre. Real systems (and your coursework's
// "Introduction to Systems Performance and Measurement" material) care
// about p50/p95/p99 precisely because tail latency is what users
// actually notice.
// ---------------------------------------------------------------------
void latencyBenchmark() {
    std::cout << "=== Part 2: order-matching latency percentiles ===\n";
    OrderBook book;
    const int N = 100000;
    std::vector<double> latenciesUs; // one measurement per order, in microseconds
    latenciesUs.reserve(N);

    std::mt19937 rng(42);
    std::uniform_real_distribution<double> priceDist(95.0, 105.0);
    std::uniform_int_distribution<int> qtyDist(1, 50);
    std::bernoulli_distribution sideCoin(0.5);

    for (int i = 0; i < N; ++i) {
        Side side = sideCoin(rng) ? Side::BUY : Side::SELL;
        auto order = OrderFactory::createOrder(
            OrderKind::LIMIT, i, 1, side, qtyDist(rng), priceDist(rng));

        auto t0 = Clock::now();
        book.addOrder(std::move(order));
        auto t1 = Clock::now();

        latenciesUs.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    std::sort(latenciesUs.begin(), latenciesUs.end());
    auto percentile = [&](double p) {
        return latenciesUs[static_cast<std::size_t>(p * (latenciesUs.size() - 1))];
    };

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Processed " << N << " orders\n";
    std::cout << "  p50 (median): " << percentile(0.50) << " us\n";
    std::cout << "  p95:          " << percentile(0.95) << " us\n";
    std::cout << "  p99:          " << percentile(0.99) << " us\n";
    std::cout << "  max:          " << latenciesUs.back() << " us\n";
    std::cout << "  (p99 vs p50 ratio: " << (percentile(0.99) / percentile(0.50))
              << "x - shows tail latency spread)\n\n";
}

int main() {
    allocationBenchmark();
    latencyBenchmark();
    return 0;
}
