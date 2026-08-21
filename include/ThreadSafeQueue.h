#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>

// A generic (templated) thread-safe queue: multiple threads can push()
// concurrently, one or more threads can pop() concurrently, and nobody
// corrupts the underlying std::queue, because every access goes through
// a mutex.
//
// COURSEWORK LINK - "Concurrency and Lightweight Threading": this is the
// textbook producer-consumer pattern. `push()` is the producer side (trader
// threads), `pop()` is the consumer side (the matching thread). The
// condition_variable is what lets pop() SLEEP when the queue is empty
// instead of spinning in a busy-loop burning CPU - the OS wakes the thread
// up only when notify_one() is called, which is far more efficient than
// polling.
template <typename T>
class ThreadSafeQueue {
public:
    void push(T item) {
        {
            // std::lock_guard: RAII for a mutex - locks on construction,
            // unlocks automatically when it goes out of scope (even if an
            // exception is thrown). Never call mtx_.lock()/unlock() by hand;
            // an exception between them would leave the mutex locked forever.
            std::lock_guard<std::mutex> lock(mtx_);
            queue_.push(std::move(item));
        }
        // notify_one() must happen AFTER releasing the lock (it happens
        // naturally here since lock_guard's destructor already ran) - waking
        // a thread that immediately blocks again trying to re-acquire a
        // still-held mutex is wasted work.
        cv_.notify_one();
    }

    // Blocking pop: sleeps until an item is available OR stop() is called.
    // Returns std::nullopt only in the shutdown case, so callers can tell
    // "got an item" apart from "queue is shutting down, stop looping."
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mtx_);
        // wait() atomically: (1) unlocks mtx_, (2) sleeps, (3) on wakeup,
        // re-locks mtx_ and checks the predicate again. The predicate
        // (the lambda) guards against SPURIOUS WAKEUPS - the OS is allowed
        // to wake a waiting thread even with no notify_one() call, so you
        // must always re-check the actual condition in a loop, which
        // std::condition_variable::wait's predicate overload does for you.
        cv_.wait(lock, [this] { return !queue_.empty() || stopping_; });

        if (queue_.empty()) {
            return std::nullopt; // we only get here if stopping_ was set
        }
        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stopping_ = true;
        }
        cv_.notify_all(); // wake up EVERY thread blocked in pop() so they
                           // can see stopping_ and exit cleanly
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mtx_);
        stopping_ = false;
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mtx_;   // "mutable" so even const methods could lock
                                // it if we added any (we don't need this yet,
                                // but it's the idiomatic declaration)
    std::condition_variable cv_;
    bool stopping_ = false;
};
