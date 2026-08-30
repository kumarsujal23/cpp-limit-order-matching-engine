#include "PostgresJournal.h"

#include <pqxx/pqxx>   // libpqxx - the official C++ PostgreSQL client
#include <exception>
#include <iostream>
#include <utility>

// ---------------------------------------------------------------------------
// Stable enum <-> integer encodings. We map explicitly (rather than casting the
// enum to its underlying value) so the on-disk format can NEVER silently change
// if someone reorders an enum in Types.h later. The database is a long-lived
// contract; the C++ enum is not.
// ---------------------------------------------------------------------------
namespace {
int         encodeCmd(CommandType t) { return t == CommandType::SUBMIT ? 0 : 1; }
CommandType decodeCmd(int v)         { return v == 0 ? CommandType::SUBMIT : CommandType::CANCEL; }
int         encodeKind(OrderKind k)  { return k == OrderKind::LIMIT ? 1 : 0; }
OrderKind   decodeKind(int v)        { return v == 1 ? OrderKind::LIMIT : OrderKind::MARKET; }
int         encodeSide(Side s)       { return s == Side::BUY ? 0 : 1; }
Side        decodeSide(int v)        { return v == 0 ? Side::BUY : Side::SELL; }
} // namespace

PostgresJournal::PostgresJournal(std::string connString)
    : connString_(std::move(connString)) {
    // Open the writer's long-lived connection and make sure the tables exist.
    // This all happens on the constructing thread BEFORE the writer thread is
    // launched, so there's no sharing of conn_ yet.
    conn_ = std::make_unique<pqxx::connection>(connString_);
    ensureSchema();

    // Prepare the two hot-path INSERTs once. After this, each write is just
    // "ship the parameters for statement X" - the server already has the plan.
    conn_->prepare("ins_event",
        "INSERT INTO events "
        "(seq,type,kind,order_id,trader_id,side,quantity,price,epoch_nanos) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9)");
    conn_->prepare("ins_trade",
        "INSERT INTO trades "
        "(buy_order_id,sell_order_id,price,quantity,epoch_nanos) "
        "VALUES ($1,$2,$3,$4,$5)");

    // Only NOW start the writer thread; from here on it has exclusive use of
    // conn_ (loadAll() deliberately uses its own separate connection).
    writerThread_ = std::thread(&PostgresJournal::writerLoop, this);
}

PostgresJournal::~PostgresJournal() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stopping_ = true;
    }
    workCv_.notify_all();
    if (writerThread_.joinable()) writerThread_.join();
    // conn_ (the unique_ptr) closes the connection as it's destroyed.
}

void PostgresJournal::ensureSchema() {
    pqxx::work txn(*conn_);

    // events = the write-ahead log: the ordered stream of INPUT commands. `seq`
    // is the primary key, which also enforces "no duplicate sequence numbers".
    txn.exec(
        "CREATE TABLE IF NOT EXISTS events ("
        "  seq         BIGINT PRIMARY KEY,"
        "  type        SMALLINT NOT NULL,"       // 0=SUBMIT 1=CANCEL
        "  kind        SMALLINT NOT NULL,"       // 0=MARKET 1=LIMIT
        "  order_id    BIGINT   NOT NULL,"
        "  trader_id   BIGINT   NOT NULL,"
        "  side        SMALLINT NOT NULL,"       // 0=BUY 1=SELL
        "  quantity    INTEGER  NOT NULL,"
        "  price       DOUBLE PRECISION NOT NULL,"
        "  epoch_nanos BIGINT   NOT NULL)");

    // trades = the OUTPUT projection: every fill, in a shape that's trivial to
    // query for analytics (volume, VWAP, P&L). NOT a recovery source - replaying
    // `events` regenerates all of these deterministically.
    txn.exec(
        "CREATE TABLE IF NOT EXISTS trades ("
        "  id            BIGSERIAL PRIMARY KEY,"
        "  buy_order_id  BIGINT  NOT NULL,"
        "  sell_order_id BIGINT  NOT NULL,"
        "  price         DOUBLE PRECISION NOT NULL,"
        "  quantity      INTEGER NOT NULL,"
        "  epoch_nanos   BIGINT  NOT NULL)");

    txn.commit();
}

// append() and recordTrade() run on the ENGINE'S MATCHING THREAD - the hot
// path. They must be cheap: take the lock, drop the record on a queue, signal
// the writer, return. No network I/O here.
void PostgresJournal::append(const JournalEvent& ev) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        eventQueue_.push_back(ev);
        ++enqueued_;
    }
    workCv_.notify_one();
}

void PostgresJournal::recordTrade(const Trade& t) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        tradeQueue_.push_back(TradeRow{t, nowEpochNanos()});
        ++enqueued_;
    }
    workCv_.notify_one();
}

void PostgresJournal::flush() {
    std::unique_lock<std::mutex> lk(mtx_);
    const std::uint64_t target = enqueued_;  // everything queued up to this moment
    workCv_.notify_one();                    // nudge the writer in case it's idle
    // Block until the writer has committed at least `target` records (or we're
    // tearing down). This is what makes flush() a real durability checkpoint.
    flushedCv_.wait(lk, [&] { return done_ >= target || stopping_; });
}

void PostgresJournal::writerLoop() {
    while (true) {
        std::deque<JournalEvent> events;
        std::deque<TradeRow>     trades;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            workCv_.wait(lk, [&] {
                return !eventQueue_.empty() || !tradeQueue_.empty() || stopping_;
            });
            if (stopping_ && eventQueue_.empty() && tradeQueue_.empty()) {
                break; // asked to stop AND nothing left to write
            }
            // Grab whatever has accumulated in one shot (swap = O(1), no copy).
            // Everything queued while we were committing the previous batch gets
            // picked up here - so a burst naturally coalesces into a big batch.
            events.swap(eventQueue_);
            trades.swap(tradeQueue_);
        }

        const std::size_t batchCount = events.size() + trades.size();
        try {
            // ONE transaction for the whole batch => ONE commit (one fsync) no
            // matter how many rows. Amortising that fsync is the entire point.
            pqxx::work txn(*conn_);
            for (const JournalEvent& ev : events) {
                txn.exec_prepared("ins_event",
                    static_cast<long long>(ev.seq),
                    encodeCmd(ev.type),
                    encodeKind(ev.kind),
                    static_cast<long long>(ev.orderId),
                    static_cast<long long>(ev.traderId),
                    encodeSide(ev.side),
                    ev.quantity,
                    ev.price,
                    static_cast<long long>(ev.epochNanos));
            }
            for (const TradeRow& tr : trades) {
                txn.exec_prepared("ins_trade",
                    static_cast<long long>(tr.trade.buyOrderId),
                    static_cast<long long>(tr.trade.sellOrderId),
                    tr.trade.price,
                    tr.trade.quantity,
                    static_cast<long long>(tr.epochNanos));
            }
            txn.commit();

            eventsWritten_ += events.size();
            tradesWritten_ += trades.size();
            ++batchesCommitted_;
        } catch (const std::exception& e) {
            // A real system would retry with backoff / a dead-letter path. Here
            // we log and continue so a transient error can't wedge the writer
            // and deadlock a flush() waiter.
            std::cerr << "[PostgresJournal] batch commit failed: " << e.what()
                      << " (dropped " << batchCount << " record(s))\n";
        }

        // Advance done_ whether we committed or failed, so flush() can proceed.
        {
            std::lock_guard<std::mutex> lk(mtx_);
            done_ += batchCount;
        }
        flushedCv_.notify_all();
    }
}

std::vector<JournalEvent> PostgresJournal::loadAll() {
    // Recovery read. Uses its OWN short-lived connection because the writer
    // thread owns conn_, and a single pqxx::connection must not be touched by
    // two threads at once. Reading past events on a fresh connection is safe and
    // simple.
    std::vector<JournalEvent> out;

    pqxx::connection c(connString_);
    pqxx::work txn(c);
    pqxx::result rows = txn.exec(
        "SELECT seq,type,kind,order_id,trader_id,side,quantity,price,epoch_nanos "
        "FROM events ORDER BY seq ASC");   // ascending seq == exact original order
    txn.commit();

    out.reserve(rows.size());
    for (const auto& row : rows) {
        JournalEvent ev;
        ev.seq        = static_cast<std::uint64_t>(row["seq"].as<long long>());
        ev.type       = decodeCmd(row["type"].as<int>());
        ev.kind       = decodeKind(row["kind"].as<int>());
        ev.orderId    = static_cast<OrderId>(row["order_id"].as<long long>());
        ev.traderId   = static_cast<TraderId>(row["trader_id"].as<long long>());
        ev.side       = decodeSide(row["side"].as<int>());
        ev.quantity   = row["quantity"].as<int>();
        ev.price      = row["price"].as<double>();
        ev.epochNanos = row["epoch_nanos"].as<long long>();
        out.push_back(ev);
    }
    return out;
}
