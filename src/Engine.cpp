#include "Engine.h"
#include "OrderFactory.h"
#include <algorithm>

Engine::Engine() = default;

Engine::~Engine() {
    stop(); // safety net: if the caller forgot stop(), don't leave a joinable
            // thread - destroying a still-joinable std::thread calls
            // std::terminate(), which would crash the program.
}

void Engine::setJournal(IJournal* journal) {
    journal_ = journal ? journal : &nullJournal_;
}

void Engine::setMarketDataSink(IMarketDataSink* sink) {
    sink_ = sink ? sink : &nullSink_;
}

void Engine::start() {
    if (running_) return;
    if (matchingThread_.joinable()) matchingThread_.join();
    incomingQueue_.reset();
    running_ = true;
    // std::thread's constructor immediately starts runMatchingLoop on a new OS
    // thread; `this` is captured so it runs as a member function on this Engine.
    matchingThread_ = std::thread(&Engine::runMatchingLoop, this);
}

void Engine::stop() {
    if (!running_) return;
    running_ = false;
    incomingQueue_.stop(); // wakes the matching thread out of its blocking pop()
    if (matchingThread_.joinable()) {
        // join() blocks until the matching thread finishes draining the queue
        // and returns. Always join a thread before its owner dies.
        matchingThread_.join();
    }
    journal_->flush(); // make sure everything we processed is durably stored
}

void Engine::submitOrder(std::unique_ptr<Order> order) {
    incomingQueue_.push(Command::submit(std::move(order)));
}

void Engine::cancelOrder(OrderId id) {
    incomingQueue_.push(Command::cancel(id));
}

// Applies a single journaled event to the book, single-threaded. Used ONLY by
// recover() - the live path in runMatchingLoop() already holds the order object
// and doesn't need to rebuild it from an event.
std::vector<Trade> Engine::applyEvent(const JournalEvent& ev) {
    if (ev.type == CommandType::SUBMIT) {
        auto order = OrderFactory::createOrder(
            ev.kind, ev.orderId, ev.traderId, ev.side, ev.quantity, ev.price);
        return book_.addOrder(std::move(order));
    }
    // CANCEL: a cancel that no longer matches anything (order already filled on
    // replay) simply returns false and is ignored - exactly as it was live.
    book_.cancelOrder(ev.orderId);
    return {};
}

void Engine::recover() {
    // Precondition: engine is stopped, so no matching thread is running and we
    // can touch book_ directly here.
    std::vector<JournalEvent> events = journal_->loadAll();

    std::uint64_t maxSeq = 0;
    for (const JournalEvent& ev : events) {
        std::vector<Trade> trades = applyEvent(ev);
        // Rebuild the trade log too, so getTradeLog() after recovery shows the
        // same history the engine produced before it went down.
        for (const Trade& t : trades) tradeLog_.push_back(t);
        maxSeq = std::max(maxSeq, ev.seq);
    }

    // Continue numbering AFTER the last recovered event, so new commands don't
    // reuse sequence numbers already in the log.
    nextSeq_.store(maxSeq + 1);
}

void Engine::runMatchingLoop() {
    while (true) {
        std::optional<Command> maybeCmd = incomingQueue_.pop();
        if (!maybeCmd.has_value()) {
            break; // pop() returns nullopt only after stop() drains the queue
        }
        Command cmd = std::move(maybeCmd.value());

        // ---- 1. WRITE-AHEAD: journal the command BEFORE applying it ----
        // Build the persistable event first (it reads the order's fields, so it
        // must happen before we move the order into the book below).
        std::uint64_t seq = nextSeq_.fetch_add(1);
        JournalEvent ev = (cmd.type == CommandType::SUBMIT)
                              ? makeSubmitEvent(*cmd.order, seq)
                              : makeCancelEvent(cmd.cancelId, seq);
        journal_->append(ev);

        // ---- 2. APPLY the command to the in-memory book ----
        std::vector<Trade> trades;
        if (cmd.type == CommandType::SUBMIT) {
            trades = book_.addOrder(std::move(cmd.order));
        } else {
            book_.cancelOrder(cmd.cancelId);
        }

        // ---- 3. PUBLISH results: trade log, market data, user callback ----
        for (const Trade& t : trades) {
            {
                std::lock_guard<std::mutex> logLock(tradeLogMutex_);
                tradeLog_.push_back(t);
            }
            journal_->recordTrade(t); // durable, queryable trades table (Postgres)
            sink_->onTrade(t);        // real-time fan-out + cache (Redis)

            std::function<void(const Trade&)> callback;
            {
                std::lock_guard<std::mutex> callbackLock(callbackMutex_);
                callback = onTrade_;
            }
            if (callback) callback(t);
        }

        // Report the new top of book after this command (cheap: O(1) map reads).
        double bid = 0.0, ask = 0.0;
        bool hasBid = book_.getBestBid(bid);
        bool hasAsk = book_.getBestAsk(ask);
        sink_->onTopOfBook(hasBid, bid, hasAsk, ask);
    }
}

std::vector<Trade> Engine::getTradeLog() const {
    std::lock_guard<std::mutex> lock(tradeLogMutex_);
    return tradeLog_; // returns a COPY - a stable snapshot, safe to read even
                       // while the matching thread keeps appending
}

void Engine::setOnTrade(std::function<void(const Trade&)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    onTrade_ = std::move(callback);
}
