// persistence_demo - the end-to-end story that ties the whole project together,
// using the REAL datastores (PostgreSQL for the durable write-ahead log, Redis
// for real-time market-data fan-out). Run it on a machine where Postgres and
// Redis are up (see docker-compose.yml).
//
// It has three clearly separated phases:
//   PHASE 1  trade for a bit, persist every command to Postgres, then "crash"
//            (drop the engine on the floor - no graceful state save).
//   PHASE 2  spin up a brand-new engine, point it at the same Postgres log,
//            recover(), and prove the book + trade history came back intact.
//   PHASE 3  push a big workload through the engine WITH full durable
//            persistence on, and print the real orders/sec + batching stats.
//
// Connection settings come from env vars so you don't have to recompile:
//   PG_CONN     (default: host=localhost port=5432 dbname=trading user=trader password=trader)
//   REDIS_HOST  (default: 127.0.0.1)
//   REDIS_PORT  (default: 6379)

#include "Engine.h"
#include "OrderFactory.h"
#include "../persistence/PostgresJournal.h"
#include "../persistence/RedisMarketData.h"

#include <pqxx/pqxx>   // only for the TRUNCATE helper below
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

using Clock = std::chrono::steady_clock;

static std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return v ? std::string(v) : fallback;
}

// Wipe the tables so every run starts clean and the recovery story is
// deterministic (otherwise old events from a previous run would replay too).
static void truncateTables(const std::string& pgConn) {
    pqxx::connection c(pgConn);
    pqxx::work txn(c);
    txn.exec("TRUNCATE TABLE events");
    txn.exec("TRUNCATE TABLE trades");
    txn.commit();
}

static void printTrades(const std::string& label, const std::vector<Trade>& trades) {
    std::cout << label << " (" << trades.size() << " trade(s)):\n";
    for (const Trade& t : trades) {
        std::cout << "   buy#" << t.buyOrderId << " x sell#" << t.sellOrderId
                  << "  qty " << t.quantity << " @ " << t.price << "\n";
    }
}

int main() {
    const std::string pgConn = envOr("PG_CONN",
        "host=localhost port=5432 dbname=trading user=trader password=trader");
    const std::string redisHost = envOr("REDIS_HOST", "127.0.0.1");
    const int redisPort = std::stoi(envOr("REDIS_PORT", "6379"));

    try {
        truncateTables(pgConn);
    } catch (const std::exception& e) {
        std::cerr << "Could not connect to Postgres (" << e.what() << ").\n"
                  << "Is the database up? Try:  docker compose up -d\n";
        return 1;
    }

    // -----------------------------------------------------------------
    // PHASE 1: trade, persist to the WAL, then simulate a crash.
    // -----------------------------------------------------------------
    std::cout << "===== PHASE 1: trade + persist, then crash =====\n";
    {
        PostgresJournal journal(pgConn);
        RedisMarketData market(redisHost, redisPort);

        Engine engine;
        engine.setJournal(&journal);        // durable log  -> Postgres
        engine.setMarketDataSink(&market);  // real-time feed -> Redis
        engine.start();

        // A tiny scripted scenario whose outcome is easy to eyeball:
        engine.submitOrder(OrderFactory::createOrder(OrderKind::LIMIT, 1, 1, Side::SELL, 10, 100.0));
        engine.submitOrder(OrderFactory::createOrder(OrderKind::LIMIT, 2, 2, Side::BUY,   4, 100.0)); // trades 4, 6 rest
        engine.submitOrder(OrderFactory::createOrder(OrderKind::LIMIT, 3, 1, Side::SELL,  5, 101.0)); // rests at 101
        engine.cancelOrder(3);                                                                        // ...then cancelled

        engine.stop(); // drains the queue and FLUSHES the journal (durable now)

        printTrades("  before crash", engine.getTradeLog());
        std::cout << "  events persisted to Postgres: " << journal.eventsWritten()
                  << " | trades persisted: " << journal.tradesWritten()
                  << " | Redis commands sent: " << market.commandsSent() << "\n";
        // engine/journal/market all destruct at the end of this scope. The
        // in-memory book is GONE - only the Postgres log survives. That's our
        // "crash".
    }
    std::cout << "  *** process 'crashed' - in-memory state discarded ***\n\n";

    // -----------------------------------------------------------------
    // PHASE 2: fresh engine, same log, recover, and prove it worked.
    // -----------------------------------------------------------------
    std::cout << "===== PHASE 2: restart + recover from the log =====\n";
    {
        PostgresJournal journal(pgConn); // same database, same events table
        Engine engine;
        engine.setJournal(&journal);
        engine.recover();                // replay every event in seq order

        printTrades("  recovered trade history", engine.getTradeLog());

        // Prove the BOOK was rebuilt, not just the trade log: order #1 should
        // still have 6 shares resting at 100, and order #3 must be gone (it was
        // cancelled before the crash).
        engine.start();
        engine.submitOrder(OrderFactory::createOrder(OrderKind::LIMIT, 4, 3, Side::BUY, 6, 100.0)); // hits recovered resting #1
        engine.submitOrder(OrderFactory::createOrder(OrderKind::LIMIT, 5, 3, Side::BUY, 5, 101.0)); // nothing at 101 -> rests
        engine.stop();

        printTrades("  after trading post-recovery", engine.getTradeLog());
        std::cout << "  (expected: the original 4-share trade, plus a new 6-share"
                     " fill against the recovered resting order)\n\n";
    }

    // -----------------------------------------------------------------
    // PHASE 3: durable throughput - the number you can actually quote.
    // -----------------------------------------------------------------
    std::cout << "===== PHASE 3: throughput WITH durable persistence =====\n";
    truncateTables(pgConn);
    {
        PostgresJournal journal(pgConn);
        RedisMarketData market(redisHost, redisPort);
        Engine engine;
        engine.setJournal(&journal);
        engine.setMarketDataSink(&market);
        engine.start();

        const int N = 100000;
        std::mt19937 rng(123);
        std::uniform_real_distribution<double> priceDist(98.0, 102.0);
        std::uniform_int_distribution<int> qtyDist(1, 20);
        std::bernoulli_distribution sideCoin(0.5);

        auto start = Clock::now();
        for (int i = 0; i < N; ++i) {
            Side side = sideCoin(rng) ? Side::BUY : Side::SELL;
            engine.submitOrder(OrderFactory::createOrder(
                OrderKind::LIMIT, i + 1, 1, side, qtyDist(rng), priceDist(rng)));
        }
        engine.stop(); // includes flushing ALL commits to Postgres
        double sec = std::chrono::duration<double>(Clock::now() - start).count();

        double avgBatch = journal.batchesCommitted()
                              ? double(journal.eventsWritten() + journal.tradesWritten()) /
                                    journal.batchesCommitted()
                              : 0.0;

        std::cout << "  submitted " << N << " orders in " << sec << " s\n";
        std::cout << "  durable throughput: " << (N / sec) << " orders/sec"
                     " (each order journaled + committed to Postgres)\n";
        std::cout << "  Postgres: " << journal.eventsWritten() << " events + "
                  << journal.tradesWritten() << " trades in "
                  << journal.batchesCommitted() << " batches"
                  << " (avg " << avgBatch << " rows/commit - this is the"
                     " group-commit batching at work)\n";
        std::cout << "  Redis commands published: " << market.commandsSent() << "\n";
    }

    std::cout << "\nDone. Inspect the durable state directly:\n"
                 "  psql -c 'SELECT count(*) FROM events;'\n"
                 "  psql -c 'SELECT sum(quantity*price) AS notional FROM trades;'\n"
                 "  redis-cli GET md:best_bid ; redis-cli GET md:last_price\n";
    return 0;
}
