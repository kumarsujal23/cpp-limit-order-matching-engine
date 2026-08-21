#include "Engine.h"
#include <iostream>

Engine::Engine() = default;

Engine::~Engine() {
    stop(); // safety net: if the user forgot to call stop(), don't leak a
            // joinable thread (std::terminate is called if a std::thread is
            // destroyed while still joinable - this avoids that crash).
}

void Engine::start() {
    if (running_) return;
    if (matchingThread_.joinable()) matchingThread_.join();
    incomingQueue_.reset();
    running_ = true;
    // std::thread's constructor immediately starts running the given
    // function on a NEW OS thread. `this` is captured so runMatchingLoop
    // runs as a member function on THIS Engine instance.
    matchingThread_ = std::thread(&Engine::runMatchingLoop, this);
}

void Engine::stop() {
    if (!running_) return;
    running_ = false;
    incomingQueue_.stop(); // wakes the matching thread out of its blocking pop()
    if (matchingThread_.joinable()) {
        // join() blocks the CALLING thread until matchingThread_ finishes.
        // Always join (or explicitly detach, which we deliberately don't do
        // here) before the Engine is destroyed - a thread you forgot about
        // is a classic source of crashes and undefined behavior.
        matchingThread_.join();
    }
}

void Engine::submitOrder(std::unique_ptr<Order> order) {
    // Just hands the order to the thread-safe queue. This function can be
    // called from many trader threads AT THE SAME TIME - that's the whole
    // point of ThreadSafeQueue's internal locking.
    incomingQueue_.push(std::move(order));
}

bool Engine::cancelOrder(OrderId id) {
    std::lock_guard<std::mutex> lock(bookMutex_);
    return book_.cancelOrder(id);
}

void Engine::runMatchingLoop() {
    while (true) {
        std::optional<std::unique_ptr<Order>> maybeOrder = incomingQueue_.pop();
        if (!maybeOrder.has_value()) {
            // pop() only returns nullopt when stop() was called - time to exit.
            break;
        }

        std::vector<Trade> trades;
        {
            std::lock_guard<std::mutex> lock(bookMutex_);
            trades = book_.addOrder(std::move(maybeOrder.value()));
        }

        if (!trades.empty()) {
            for (const auto& t : trades) {
                {
                    std::lock_guard<std::mutex> logLock(tradeLogMutex_);
                    tradeLog_.push_back(t);
                }

                std::function<void(const Trade&)> callback;
                {
                    std::lock_guard<std::mutex> callbackLock(callbackMutex_);
                    callback = onTrade_;
                }
                if (callback) callback(t);
            }
        }
    }
}

std::vector<Trade> Engine::getTradeLog() const {
    std::lock_guard<std::mutex> lock(tradeLogMutex_);
    return tradeLog_; // returns a COPY - caller gets a stable snapshot,
                       // safe even while the matching thread keeps running
}

void Engine::setOnTrade(std::function<void(const Trade&)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    onTrade_ = std::move(callback);
}
