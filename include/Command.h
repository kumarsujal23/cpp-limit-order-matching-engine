#pragma once
#include "Types.h"
#include "Order.h"
#include <memory>

// A Command is a single unit of work flowing INTO the engine. Both "submit a
// new order" and "cancel a resting order" travel down the SAME queue as a
// Command, so the matching thread processes them in one strictly ordered
// stream. That single ordering is important for two reasons:
//
//   1. Determinism - a cancel that arrives before a matching buy must be seen
//      by the book in that order, every run, regardless of thread timing.
//   2. Recovery - because the engine journals each Command in the order it
//      processed them, replaying that journal reproduces the exact same book
//      and the exact same trades. (This is the core idea behind "event
//      sourcing": store the input events, not the derived state.)
//
// Design note worth explaining in an interview: an earlier version of this
// engine let cancelOrder() touch the book directly from the caller's thread
// under a mutex, "so a cancel wouldn't wait behind a backlog of new orders."
// That was faster for the cancel but broke the single-ordered-stream property
// above - two threads mutating the book meant the outcome could depend on lock
// timing, and there was no single ordered log to replay for recovery. Routing
// cancels through the same queue trades a little cancel latency for
// determinism AND recoverability, which is the right call for something that
// has to be auditable.
struct Command {
    CommandType type;

    // Valid only when type == SUBMIT. The Command OWNS the order until the
    // matching thread moves it into the book. unique_ptr makes that ownership
    // transfer explicit and leak-free (RAII).
    std::unique_ptr<Order> order;

    // Valid only when type == CANCEL.
    OrderId cancelId = 0;

    // Convenience factory helpers so call sites read clearly.
    static Command submit(std::unique_ptr<Order> o) {
        return Command{CommandType::SUBMIT, std::move(o), 0};
    }
    static Command cancel(OrderId id) {
        return Command{CommandType::CANCEL, nullptr, id};
    }
};
