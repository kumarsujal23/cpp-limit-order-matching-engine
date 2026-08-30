#pragma once
#include "IJournal.h"
#include <string>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cstdint>
#include <memory>

// Forward-declare so this header does NOT drag <pqxx/pqxx> into every file that
// includes it - only PostgresJournal.cpp needs the libpqxx headers. (Pointer to
// an incomplete type is fine as long as the destructor lives in the .cpp.)
namespace pqxx { class connection; }

// PostgresJournal is the production implementation of IJournal: a durable,
// queryable write-ahead log backed by PostgreSQL.
//
// KEY DESIGN POINT (this is the part worth explaining in an interview):
// the matching thread must never block on a database round-trip. So append()
// and recordTrade() do NOT talk to Postgres directly - they drop the record
// onto an in-process queue and return immediately. A dedicated BACKGROUND
// WRITER THREAD drains that queue and writes in BATCHES, committing many
// records per transaction. Batching matters because the expensive part of a
// durable write is the commit (an fsync to disk); amortising one commit over
// hundreds of rows is what turns "a few thousand writes/sec" into "hundreds of
// thousands". This is exactly the "asynchronous group commit" pattern real
// databases (including Postgres itself) use internally.
//
// Durability trade-off, stated honestly: because writes are asynchronous,
// a hard crash can lose the last un-committed batch. flush() forces a commit
// and blocks until it is durable - use it as a checkpoint before a clean
// shutdown. A strict zero-loss WAL would fsync synchronously on the hot path
// and pay the latency for it; this project deliberately chooses throughput +
// checkpoints, which is the right call for most systems and a real, defensible
// engineering decision to talk about.
class PostgresJournal : public IJournal {
public:
    // connString is a libpq connection string, e.g.
    //   "host=localhost port=5432 dbname=trading user=trader password=trader"
    explicit PostgresJournal(std::string connString);
    ~PostgresJournal() override;

    PostgresJournal(const PostgresJournal&) = delete;
    PostgresJournal& operator=(const PostgresJournal&) = delete;

    void append(const JournalEvent& ev) override;   // enqueue (non-blocking)
    void recordTrade(const Trade& t) override;       // enqueue (non-blocking)
    void flush() override;                            // block until durable
    std::vector<JournalEvent> loadAll() override;     // for crash recovery

    // Simple counters so the benchmarks/demos can print real persistence stats.
    std::uint64_t eventsWritten()   const { return eventsWritten_.load(); }
    std::uint64_t tradesWritten()   const { return tradesWritten_.load(); }
    std::uint64_t batchesCommitted() const { return batchesCommitted_.load(); }

private:
    // A trade plus the wall-clock time it was recorded (Trade's own timestamp
    // is a steady_clock point, which isn't meaningful once persisted).
    struct TradeRow {
        Trade trade;
        std::int64_t epochNanos;
    };

    void writerLoop();      // body of the background writer thread
    void ensureSchema();    // CREATE TABLE IF NOT EXISTS on startup

    std::string connString_;
    std::unique_ptr<pqxx::connection> conn_; // owned by the writer thread after ctor

    std::mutex mtx_;
    std::condition_variable workCv_;    // writer waits here for work / shutdown
    std::condition_variable flushedCv_; // flush() waiters wait here
    std::deque<JournalEvent> eventQueue_;
    std::deque<TradeRow>     tradeQueue_;
    bool stopping_ = false;

    // "enqueued" counts everything handed to append()/recordTrade(); "done"
    // counts everything actually committed. flush() waits for done >= enqueued.
    std::uint64_t enqueued_ = 0;
    std::uint64_t done_ = 0;

    std::atomic<std::uint64_t> eventsWritten_{0};
    std::atomic<std::uint64_t> tradesWritten_{0};
    std::atomic<std::uint64_t> batchesCommitted_{0};

    std::thread writerThread_;
};
