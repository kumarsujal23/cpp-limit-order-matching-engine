#pragma once
#include "OrderBook.h"
#include "Command.h"
#include "IJournal.h"
#include "IMarketDataSink.h"
#include "ThreadSafeQueue.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>

// Engine owns exactly ONE OrderBook and runs exactly ONE background thread
// that does all the matching. Any number of caller threads can call
// submitOrder()/cancelOrder() concurrently - each just pushes a Command onto
// the thread-safe queue and returns immediately (an "asynchronous" API: the
// caller doesn't wait for the match to actually happen).
//
// WHY single-threaded matching, when we HAVE multiple cores available?
// This is a deliberate, informed design choice, not a limitation:
//   1. Determinism - the outcome must not depend on thread scheduling luck.
//      Two threads matching against the SAME book concurrently could interleave
//      in ways that produce different (and both "valid-looking") results
//      depending on timing - unacceptable for something legally auditable.
//   2. Because everything the engine does is driven by ONE ordered stream of
//      Commands, we can write that stream to a journal and REPLAY it to rebuild
//      the exact book after a crash (see recover()). You cannot cleanly replay
//      a log of events that were applied by several threads in a
//      timing-dependent order.
//   3. Real exchanges' matching cores are typically single-threaded per
//      symbol - the parallelism comes from running many INDEPENDENT order
//      books (one per symbol) on different cores, not from parallelizing the
//      matching of one book.
//
// NOTE ON LOCKING: because submit AND cancel both flow through the queue, the
// matching thread is the ONLY thread that ever touches book_. That means
// book_ needs no mutex of its own - the queue is the synchronization. (This is
// the same insight behind the LMAX Disruptor: serialize writes through one
// thread and the shared state becomes lock-free by construction.) The only
// mutexes left guard data genuinely shared with caller threads: the trade log
// and the trade callback.
class Engine {
public:
    Engine();
    ~Engine();

    // Non-copyable, non-movable: an Engine owns a running thread and mutexes -
    // copying those has no sane meaning, so we delete both rather than let the
    // compiler generate a broken one. Standard practice for a thread-owning class.
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // ---- Optional plug-ins. Both must be set BEFORE start()/recover(). ----
    // If left unset, the engine uses no-op defaults (no persistence, no
    // market-data publishing) - so the pure in-memory engine still works with
    // zero dependencies, which is what the unit tests rely on.
    void setJournal(IJournal* journal);
    void setMarketDataSink(IMarketDataSink* sink);

    // Rebuild the book from the journal BEFORE starting the matching thread.
    // Reads every past command in sequence order and re-applies it, so the
    // book and trade log end up byte-for-byte what they were before the crash.
    // Safe to call only when the engine is stopped (it touches the book
    // single-threaded).
    void recover();

    void start();
    void stop();

    // Called from ANY thread. Thread-safe by construction - only touches the
    // ThreadSafeQueue.
    void submitOrder(std::unique_ptr<Order> order);

    // Asynchronous cancel: enqueues a CANCEL command that the matching thread
    // applies in stream order. Returns void (not bool) precisely because it is
    // async - whether the order was still resting is decided later, on the
    // matching thread, and shows up in the journal/trade history. (OrderBook's
    // own cancelOrder() is still synchronous and returns bool for direct use.)
    void cancelOrder(OrderId id);

    // Snapshot of all trades executed so far. Copies the vector under a lock so
    // the caller gets a consistent view, not one being mutated mid-read.
    std::vector<Trade> getTradeLog() const;

    // Optional callback fired (from the matching thread!) on every trade - lets
    // a demo/benchmark observe trades live without polling.
    void setOnTrade(std::function<void(const Trade&)> callback);

private:
    void runMatchingLoop();                       // body of the matching thread
    std::vector<Trade> applyEvent(const JournalEvent& ev); // used only by recover()

    OrderBook book_;                              // touched ONLY by the matching thread
    ThreadSafeQueue<Command> incomingQueue_;
    std::thread matchingThread_;
    std::atomic<bool> running_{false};

    // Monotonic sequence number stamped onto each journaled command. Assigned
    // only by the matching thread, so a plain counter would do, but atomic
    // keeps recover()/reads honest and costs nothing measurable here.
    std::atomic<std::uint64_t> nextSeq_{1};

    // Default no-op plug-ins. Declared BEFORE the pointers below so the
    // in-class initializers can point at them (member init order = declaration
    // order).
    NullJournal nullJournal_;
    NullMarketDataSink nullSink_;
    IJournal* journal_ = &nullJournal_;
    IMarketDataSink* sink_ = &nullSink_;

    mutable std::mutex tradeLogMutex_;
    std::vector<Trade> tradeLog_;

    mutable std::mutex callbackMutex_;
    std::function<void(const Trade&)> onTrade_;
};
