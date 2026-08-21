#include "Engine.h"
#include "OrderFactory.h"
#include "ThreadSafeQueue.h"
#include "LockFreeSPSCQueue.h"
#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <chrono>
#include <atomic>

// ---------------------------------------------------------------------
// PART 1: Multiple trader threads submitting orders concurrently into
// the Engine, matched safely by one dedicated matching thread.
// ---------------------------------------------------------------------
void concurrentEngineDemo() {
    std::cout << "=== Part 1: concurrent order submission ===\n";

    Engine engine;
    std::atomic<int> tradeCount{0};
    engine.setOnTrade([&](const Trade&) { tradeCount++; }); // called from
                                                              // the matching
                                                              // thread - only
                                                              // touches an
                                                              // atomic, so
                                                              // this is safe
    engine.start();

    const int kTraderThreads = 4;
    const int kOrdersPerThread = 500;
    std::vector<std::thread> traders;

    // Each "trader thread" independently generates random limit orders
    // clustered around price 100, and fires them at the engine. On a
    // multi-core machine, these threads genuinely run in parallel; the
    // Engine's queue is what makes that safe despite everyone hammering
    // submitOrder() at once.
    for (int t = 0; t < kTraderThreads; ++t) {
        traders.emplace_back([&, t] {
            std::mt19937 rng(t); // seed differs per thread -> different order streams
            std::uniform_real_distribution<double> priceDist(98.0, 102.0);
            std::uniform_int_distribution<int> qtyDist(1, 20);
            std::bernoulli_distribution sideCoin(0.5);

            for (int i = 0; i < kOrdersPerThread; ++i) {
                OrderId id = static_cast<OrderId>(t) * 1'000'000 + i; // unique across threads
                Side side = sideCoin(rng) ? Side::BUY : Side::SELL;
                engine.submitOrder(OrderFactory::createOrder(
                    OrderKind::LIMIT, id, /*traderId=*/t, side, qtyDist(rng), priceDist(rng)));
            }
        });
    }

    for (auto& th : traders) th.join(); // wait for all trader threads to finish SUBMITTING
    engine.stop();                       // then let the matching thread drain the queue and stop

    std::cout << "Submitted " << (kTraderThreads * kOrdersPerThread) << " orders from "
              << kTraderThreads << " concurrent threads\n";
    std::cout << "Trades executed: " << tradeCount.load() << "\n";
    std::cout << "Trade log size (from Engine): " << engine.getTradeLog().size() << "\n\n";
}

// ---------------------------------------------------------------------
// PART 2: benchmark ThreadSafeQueue (mutex-based) vs LockFreeSPSCQueue
// for a classic single-producer/single-consumer handoff. This compares queue
// transport performance only; it does not compare matching results.
// ---------------------------------------------------------------------
template <typename PushFn, typename PopFn>
double benchmarkQueue(const std::string& label, int itemCount, PushFn push, PopFn pop) {
    auto start = std::chrono::steady_clock::now();

    std::thread producer([&] {
        for (int i = 0; i < itemCount; ++i) push(i);
    });
    int received = 0;
    std::thread consumer([&] {
        while (received < itemCount) {
            if (pop()) received++;
        }
    });
    producer.join();
    consumer.join();

    auto end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    std::cout << label << ": " << itemCount << " items in " << ms << " ms  ("
              << (itemCount / (ms / 1000.0)) << " items/sec)\n";
    return ms;
}

void queueBenchmark() {
    std::cout << "=== Part 2: mutex queue vs lock-free queue throughput ===\n";
    const int kItems = 200000;

    {
        ThreadSafeQueue<int> q;
        benchmarkQueue("ThreadSafeQueue (mutex)", kItems,
            [&](int v) { q.push(v); },
            [&]() { return q.pop().has_value(); });
    }
    {
        LockFreeSPSCQueue<int, 1 << 16> q; // capacity must be power of two
        benchmarkQueue("LockFreeSPSCQueue     ", kItems,
            [&](int v) { while (!q.push(v)) { /* spin until there's room */ } },
            [&]() { return q.pop().has_value(); });
    }

    std::cout << "\nNote: this sandbox has 1 CPU core, so both queues mostly\n"
              << "time-slice rather than run truly in parallel - run this on\n"
              << "your own multi-core machine for more representative results.\n"
              << "This benchmark measures queue handoff only, not matching correctness.\n";
}

int main() {
    concurrentEngineDemo();
    queueBenchmark();
    return 0;
}
