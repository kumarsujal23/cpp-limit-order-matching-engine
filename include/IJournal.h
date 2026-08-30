#pragma once
#include "JournalEvent.h"
#include "Trade.h"
#include <vector>
#include <cstddef>

// IJournal is the write-ahead log (WAL) abstraction: an append-only record of
// every input command the engine processed, used to rebuild state after a
// crash or restart.
//
// WHY AN INTERFACE (abstract base) INSTEAD OF HARD-CODING PostgreSQL:
// the Engine should not care WHERE the log lives. In production it's a
// PostgresJournal (durable, queryable). In unit tests it's an InMemoryJournal
// (fast, no server needed) so we can prove the recovery logic is correct
// without spinning up a database in CI. This is the Dependency Inversion
// Principle - the Engine depends on the abstraction IJournal, and the concrete
// storage is plugged in from outside. It's also what lets the whole recovery
// story be TESTED even on a machine with no database installed.
class IJournal {
public:
    virtual ~IJournal() = default;

    // Append one event to the log. The caller (the engine's matching thread)
    // fills in ev.seq. Implementations are allowed to buffer/batch internally
    // for throughput, so appended data is not necessarily durable yet - call
    // flush() when you need that guarantee.
    virtual void append(const JournalEvent& ev) = 0;

    // Block until everything appended so far is durably stored. Used as a
    // checkpoint (e.g. before a clean shutdown, or before we deliberately
    // simulate a crash in the recovery demo).
    virtual void flush() = 0;

    // Read the ENTIRE log back in ascending seq order - the input to recovery.
    virtual std::vector<JournalEvent> loadAll() = 0;

    // OPTIONAL: durably record an executed trade. This is an OUTPUT PROJECTION,
    // not part of the recovery source of truth (replaying the event log
    // regenerates every trade). It exists purely so trades land in a queryable
    // table for analytics - e.g. "SELECT sum(quantity*price) FROM trades".
    // Default is a no-op, so journals that don't care (Null/InMemory) ignore it.
    virtual void recordTrade(const Trade&) {}
};

// A journal that throws everything away. Used to measure the engine's
// throughput/latency with persistence DISABLED, so we can quote the cost of
// journaling as a clean before/after number. ("Persistence off" is a legit
// configuration, not a fake database.)
class NullJournal : public IJournal {
public:
    void append(const JournalEvent&) override {}
    void flush() override {}
    std::vector<JournalEvent> loadAll() override { return {}; }
};

// An in-memory journal used as a TEST DOUBLE. It stores events in a plain
// vector, which lets tests exercise the exact same append -> loadAll -> replay
// path that PostgresJournal uses, but with zero external dependencies. Only
// the single matching thread appends to it, so no locking is required.
class InMemoryJournal : public IJournal {
public:
    void append(const JournalEvent& ev) override { events_.push_back(ev); }
    void flush() override {}
    std::vector<JournalEvent> loadAll() override { return events_; }

    std::size_t size() const { return events_.size(); }

private:
    std::vector<JournalEvent> events_;
};
