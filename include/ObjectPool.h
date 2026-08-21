#pragma once
#include <vector>
#include <cstddef>
#include <new>       // placement new
#include <stdexcept>
#include <type_traits>

// AN OBJECT POOL: pre-allocate a big block of raw memory up front, sized for
// `capacity` objects of type T. "Allocating" an object afterwards just hands
// out a pointer into that block (no call to the OS allocator); "freeing" it
// just marks that slot reusable again.
//
// WHY THIS MATTERS FOR THE "GARBAGE COLLECTION" PART OF YOUR COURSEWORK:
// C++ has no GC - you either call `new`/`delete` yourself (or let smart
// pointers do it for you, which still calls the general-purpose heap
// allocator underneath) or you manage memory some other way. A general
// `new`/`delete` has real, variable-latency overhead: the allocator has to
// search for a free block, update internal bookkeeping, maybe talk to the
// OS. In a system doing thousands of allocations per second where LATENCY
// CONSISTENCY matters (a trading engine is a textbook example - you don't
// want your 99th-percentile order-processing time to spike because the
// allocator got unlucky), pooling trades a bit of complexity for
// (a) speed - no allocator bookkeeping, just pointer arithmetic, and
// (b) predictability - no surprise slow allocation, ever, because the
// memory was already reserved at startup.
// This is the SAME MOTIVATION that makes GC languages (Java, Go) sometimes
// unsuitable for the hardest low-latency systems: a GC pause is an
// unpredictable latency spike outside your control, just like a slow
// allocator call - except worse, because it can stop ALL threads briefly.
// C++'s answer isn't "no automatic memory management," it's "you get to
// CHOOSE the allocation strategy that fits your latency requirements" -
// pooling here is exactly that choice, made deliberately.
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(std::size_t capacity)
        : storage_(capacity) // one big aligned raw buffer, allocated ONCE
    {
        freeList_.reserve(capacity);
        // Carve the raw buffer into `capacity` equally-sized slots and
        // record each slot's address as "available."
        for (std::size_t i = 0; i < capacity; ++i) {
            freeList_.push_back(reinterpret_cast<T*>(&storage_[i]));
        }
    }

    // Variadic template + perfect forwarding: acquire() can accept ANY
    // constructor arguments T supports, exactly as if you'd called
    // `new T(args...)` - but it places the object into pool memory instead
    // of asking the heap allocator for fresh space.
    template <typename... Args>
    T* acquire(Args&&... args) {
        if (freeList_.empty()) {
            throw std::runtime_error("ObjectPool exhausted - increase capacity");
        }
        T* slot = freeList_.back();
        freeList_.pop_back();

        // PLACEMENT NEW: construct a T at an address we already own, instead
        // of asking the allocator for new memory. This is the mechanism that
        // makes pooling possible - normal `new T(...)` always does both
        // "find memory" AND "construct"; placement new lets us separate them.
        return new (slot) T(std::forward<Args>(args)...);
    }

    void release(T* obj) {
        obj->~T(); // must call the destructor MANUALLY - placement new means
                   // the pool, not the runtime, owns the object's lifetime,
                   // so nothing else will ever call this for us.
        freeList_.push_back(obj);
    }

    std::size_t available() const { return freeList_.size(); }

private:
    using StorageSlot = std::aligned_storage_t<sizeof(T), alignof(T)>;

    std::vector<StorageSlot> storage_; // the pool's raw memory, owned for its
                                       // entire lifetime - no per-object
                                       // heap calls after construction
    std::vector<T*> freeList_;
};
