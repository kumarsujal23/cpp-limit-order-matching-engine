#pragma once
#include "IMarketDataSink.h"
#include <string>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cstdint>

// Forward-declare the hiredis context so this header doesn't pull in
// <hiredis/hiredis.h> everywhere - only the .cpp needs it. (hiredis defines
// redisContext as a struct, so a pointer to the incomplete struct is fine.)
struct redisContext;

// RedisMarketData is the production IMarketDataSink: it pushes the engine's
// real-time output into Redis so any number of external clients (dashboards,
// bots, loggers) can consume it without ever touching the matching engine.
//
// Two Redis usage patterns, each chosen on purpose:
//   * PUB/SUB ("PUBLISH md:trades ...") = fire-and-forget fan-out. Whoever is
//     subscribed right now sees the trade instantly. Perfect for live feeds.
//   * KEY CACHE ("SET md:best_bid ...") = last-known-value store. A client that
//     just connected can GET the current top of book in O(1) without replaying
//     history. This is the "Redis as a fast read cache" pattern.
//
// WHY REDIS AND NOT POSTGRES FOR THIS:
//   Postgres is the durable system of record (must survive a crash, pays fsync
//   cost). Redis is an in-memory cache/broker - microsecond reads, but not the
//   source of truth. Splitting "never lose it" (Postgres) from "everyone needs
//   it right now" (Redis) is a standard, defensible architecture, and it's the
//   honest answer to "why two datastores?".
//
// HOT-PATH SAFETY: onTrade()/onTopOfBook() are called ON THE MATCHING THREAD.
// They must not block on the network, so they only enqueue a message and
// return. A dedicated publisher thread owns the Redis connection and does all
// the I/O - and it PIPELINES a whole batch of commands per wake, amortising the
// network round-trip exactly like the Postgres journal amortises its fsync.
class RedisMarketData : public IMarketDataSink {
public:
    RedisMarketData(std::string host, int port);
    ~RedisMarketData() override;

    RedisMarketData(const RedisMarketData&) = delete;
    RedisMarketData& operator=(const RedisMarketData&) = delete;

    void onTrade(const Trade& trade) override;                       // enqueue only
    void onTopOfBook(bool hasBid, double bid, bool hasAsk, double ask) override;

    // Total Redis commands successfully sent - handy for demo/benchmark output.
    std::uint64_t commandsSent() const { return commandsSent_.load(); }

private:
    // A queued unit of work. One struct covers both message kinds to keep the
    // queue simple; `kind` says which fields are meaningful.
    struct Msg {
        enum Kind { TRADE, BOOK } kind;
        // TRADE fields
        std::uint64_t buyId = 0;
        std::uint64_t sellId = 0;
        double price = 0.0;
        int quantity = 0;
        std::int64_t epochNanos = 0;
        // BOOK fields
        bool hasBid = false;
        bool hasAsk = false;
        double bid = 0.0;
        double ask = 0.0;
    };

    void publisherLoop();
    bool ensureConnected(); // (re)establish ctx_ if needed; returns true if usable

    std::string host_;
    int port_;
    redisContext* ctx_ = nullptr; // owned exclusively by the publisher thread

    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Msg> queue_;
    bool stopping_ = false;

    std::atomic<std::uint64_t> commandsSent_{0};
    std::thread publisherThread_;
};
