#pragma once
#include <atomic>
#include <cstddef>
#include <optional>
#include <vector>

// A LOCK-FREE single-producer, single-consumer (SPSC) ring buffer.
//
// WHY "lock-free" instead of the mutex-based ThreadSafeQueue above? A mutex
// makes a thread SLEEP (via the OS) when it can't get the lock - correct,
// but there's real overhead: a context switch, and if a thread holding the
// lock gets descheduled by the OS, EVERY other thread waiting on that lock
// stalls too ("priority inversion" / lock convoy). Lock-free code instead
// uses atomic CPU instructions so multiple threads can never fully block
// each other - at least one thread is always making progress. This matters
// most in exactly the kind of low-latency system a trading engine is.
//
// IMPORTANT HONESTY NOTE (say this in an interview - it shows real
// understanding, not just buzzwords): general lock-free MULTI-producer /
// multi-consumer queues are genuinely hard to get right (ABA problem,
// memory reclamation). SPSC - exactly one producer thread, exactly one
// consumer thread - is the well-understood, safe-to-hand-roll case, which
// is why that's what we build here. If you needed multiple producers, the
// honest answer is "use a well-tested library (e.g. moodycamel::ConcurrentQueue)
// rather than hand-roll it," and knowing that limitation IS the systems
// knowledge being tested.
template <typename T, std::size_t Capacity>
class LockFreeSPSCQueue {
    static_assert(Capacity >= 2, "Capacity must be at least two");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two (lets us use & instead of % to wrap indices - much cheaper)");
public:
    LockFreeSPSCQueue() : buffer_(Capacity) {}

    // Called ONLY from the producer thread.
    bool push(T item) {
        std::size_t head = head_.load(std::memory_order_relaxed);
        std::size_t nextHead = (head + 1) & (Capacity - 1);

        // Read tail_ with ACQUIRE ordering: this synchronizes-with the
        // RELEASE store the consumer does in pop() below, guaranteeing we
        // see the consumer's most recent progress, not a stale cached value.
        if (nextHead == tail_.load(std::memory_order_acquire)) {
            return false; // queue is full
        }

        buffer_[head] = std::move(item);

        // RELEASE store: everything written above (buffer_[head] = ...)
        // becomes VISIBLE to the consumer thread once it does an ACQUIRE
        // load of head_ and sees this new value. Without this ordering, the
        // consumer could see the updated head_ but stale/uninitialized data
        // in buffer_[head] - reordering is legal and does happen on real
        // hardware/compilers without explicit ordering.
        head_.store(nextHead, std::memory_order_release);
        return true;
    }

    // Called ONLY from the consumer thread.
    std::optional<T> pop() {
        std::size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt; // queue is empty
        }

        T item = std::move(buffer_[tail]);
        std::size_t nextTail = (tail + 1) & (Capacity - 1);
        tail_.store(nextTail, std::memory_order_release);
        return item;
    }

private:
    std::vector<T> buffer_;
    // alignas(64): pads each atomic onto its own CPU cache line (typically
    // 64 bytes). Without this, head_ and tail_ - written by DIFFERENT
    // threads - could share one cache line, causing "false sharing": every
    // write by the producer invalidates the consumer's cached copy of that
    // line and vice versa, even though they're logically touching different
    // variables. This is a real, measurable performance bug in naive
    // lock-free code, and knowing the term "false sharing" is a strong
    // signal in a systems interview.
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};
