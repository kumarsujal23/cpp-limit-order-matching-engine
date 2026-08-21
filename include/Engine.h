#pragma once
#include "OrderBook.h"
#include "ThreadSafeQueue.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>

// Engine owns exactly ONE OrderBook and runs exactly ONE background thread
// that does all the matching. Any number of caller threads can call
// submitOrder() concurrently - it just pushes onto the thread-safe queue and
// returns immediately (this is called an "asynchronous" API: the caller
// doesn't wait for the match to actually happen).
//
// WHY single-threaded matching, when we HAVE multiple cores available?
// This is a deliberate, informed design choice, not a limitation:
//   1. Determinism - the outcome must not depend on thread scheduling luck.
//      Two threads matching different orders against the SAME book
//      concurrently could interleave in ways that produce different (and
//      both "valid-looking") results depending on timing - unacceptable for
//      something legally auditable like a trade.
//   2. It also happens to be simple to prove correct, which is why real
//      exchanges' matching cores are typically single-threaded per
//      instrument/symbol - the parallelism instead comes from running many
//      INDEPENDENT order books (one per stock symbol) on different cores,
//      not from parallelizing matching *within* one book. Multicore
//      Programming applies at the "many symbols" level, not inside one
//      order book's matching loop.
class Engine {
public:
    Engine();
    ~Engine();

    // Non-copyable, non-movable: an Engine owns a running thread and mutexes -
    // copying those doesn't have sane semantics, so we explicitly delete
    // both. This is good practice for any class managing a thread: make the
    // compiler refuse a copy rather than silently generating a broken one.
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void start();
    void stop();

    // Called from ANY thread (trader threads). Thread-safe by construction,
    // since it only touches the ThreadSafeQueue.
    void submitOrder(std::unique_ptr<Order> order);
    bool cancelOrder(OrderId id); // note: cancellation still needs to be
                                    // serialized through the matching thread
                                    // for correctness - see .cpp

    // Snapshot of all trades executed so far. Copies the vector under a lock
    // so the caller gets a consistent view, not one being mutated mid-read.
    std::vector<Trade> getTradeLog() const;

    // Optional callback fired (from the matching thread!) every time a trade
    // executes - lets a demo/benchmark observe trades live without polling.
    void setOnTrade(std::function<void(const Trade&)> callback);

private:
    void runMatchingLoop(); // the body of the dedicated matching thread

    // bookMutex_ protects book_ itself. New orders normally reach book_ only
    // through the single matching thread (so they wouldn't strictly need a
    // lock), but cancelOrder() is a deliberate exception: a trader wants
    // their cancel to take effect immediately, not wait in the same queue
    // behind a backlog of new orders. So cancelOrder() takes this mutex and
    // touches book_ directly from the CALLER's thread, while the matching
    // thread also takes it briefly while applying a popped order. This is a
    // small, honest example of mixing "single writer via queue" with
    // "shared state via mutex" where each is the right tool for that one
    // operation - worth being able to explain the trade-off in an interview.
    mutable std::mutex bookMutex_;
    OrderBook book_;
    ThreadSafeQueue<std::unique_ptr<Order>> incomingQueue_;
    std::thread matchingThread_;
    std::atomic<bool> running_{false};

    mutable std::mutex tradeLogMutex_;
    std::vector<Trade> tradeLog_;

    mutable std::mutex callbackMutex_;
    std::function<void(const Trade&)> onTrade_;
};
