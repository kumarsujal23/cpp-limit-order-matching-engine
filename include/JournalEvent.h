#pragma once
#include "Types.h"
#include "Order.h"
#include <cstdint>

// A JournalEvent is the PERSISTABLE form of a Command - a flat, plain-old-data
// record with no pointers, so it maps directly onto a database row (or a line
// in a file) and can be read back long after the original Order object is
// gone.
//
// Two deliberate differences from Command:
//   * It carries a `seq` - a monotonically increasing sequence number that
//     defines the global total order of everything the engine has processed.
//     Replaying events in `seq` order reproduces the book exactly.
//   * It stores `epochNanos` from the WALL clock (std::chrono::system_clock),
//     not the steady_clock used inside Order. steady_clock is perfect for
//     measuring durations but its zero point is arbitrary and resets across
//     runs, so it's meaningless once persisted; a wall-clock timestamp is what
//     you actually want in an audit log.
//
// Only INPUT events (SUBMIT / CANCEL) are journaled. We do NOT need to journal
// the trades themselves for recovery: because matching is deterministic,
// replaying the inputs regenerates every trade. (Trades are still written to a
// separate table for querying/analytics - that's an output projection, not the
// source of truth.)
struct JournalEvent {
    std::uint64_t seq = 0;
    CommandType   type = CommandType::SUBMIT;

    // Order fields - all meaningful when type == SUBMIT. For CANCEL only
    // `orderId` matters (the id to cancel).
    OrderKind kind = OrderKind::LIMIT;
    OrderId   orderId = 0;
    TraderId  traderId = 0;
    Side      side = Side::BUY;
    int       quantity = 0;
    double    price = 0.0;      // 0 for market orders

    std::int64_t epochNanos = 0;
};

// Current wall-clock time in nanoseconds since the Unix epoch - what we stamp
// onto an event as it is journaled.
inline std::int64_t nowEpochNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Build the journal record for a freshly submitted order. Called BEFORE the
// order is matched, so getQuantity() (the original size) is exactly what we
// want to store for replay.
inline JournalEvent makeSubmitEvent(const Order& o, std::uint64_t seq) {
    JournalEvent ev;
    ev.seq = seq;
    ev.type = CommandType::SUBMIT;
    ev.kind = o.getOrderKind();
    ev.orderId = o.getId();
    ev.traderId = o.getTraderId();
    ev.side = o.getSide();
    ev.quantity = o.getQuantity();
    // getPrice() throws for a market order, so only ask a limit order for it.
    ev.price = (ev.kind == OrderKind::LIMIT) ? o.getPrice() : 0.0;
    ev.epochNanos = nowEpochNanos();
    return ev;
}

inline JournalEvent makeCancelEvent(OrderId id, std::uint64_t seq) {
    JournalEvent ev;
    ev.seq = seq;
    ev.type = CommandType::CANCEL;
    ev.orderId = id;
    ev.epochNanos = nowEpochNanos();
    return ev;
}
