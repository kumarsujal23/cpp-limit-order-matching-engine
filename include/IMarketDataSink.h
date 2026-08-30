#pragma once
#include "Trade.h"

// IMarketDataSink is where the engine publishes what the OUTSIDE WORLD wants to
// see in real time: executed trades and the current top of book. It is
// deliberately separate from IJournal (the durable log) because the two have
// different jobs:
//   * IJournal  = "never lose this" (durability, recovery)   -> PostgreSQL
//   * IMarketDataSink = "tell everyone right now" (low latency fan-out, cache)
//                                                             -> Redis
//
// Keeping this an interface means the Engine has no idea Redis exists - a
// RedisMarketData adapter implements this in production, and tests use a
// recording/no-op double. Same Dependency Inversion idea as IJournal.
//
// Implementations must be safe to call from the matching thread and must NOT
// block it on slow network I/O - the RedisMarketData adapter therefore hands
// work to its own background thread, so onTrade()/onTopOfBook() just enqueue
// and return immediately, keeping the matching hot path fast.
class IMarketDataSink {
public:
    virtual ~IMarketDataSink() = default;

    // Called once per executed trade, in trade order.
    virtual void onTrade(const Trade& trade) = 0;

    // Called after each command is applied, with the new best bid/ask. The
    // has* flags are false when that side of the book is empty (so there is no
    // meaningful price to report).
    virtual void onTopOfBook(bool hasBid, double bid, bool hasAsk, double ask) = 0;
};

// Publishes nothing. Default sink when market-data distribution is disabled.
class NullMarketDataSink : public IMarketDataSink {
public:
    void onTrade(const Trade&) override {}
    void onTopOfBook(bool, double, bool, double) override {}
};
