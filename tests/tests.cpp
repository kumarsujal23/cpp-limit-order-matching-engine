#include "TestFramework.h"
#include "Engine.h"
#include "OrderBook.h"
#include "OrderFactory.h"
#include "IJournal.h"

TEST(no_match_when_resting_alone) {
    OrderBook book;
    auto trades = book.addOrder(OrderFactory::createOrder(
        OrderKind::LIMIT, 1, 1, Side::SELL, 10, 100.0));
    CHECK_EQ(trades.size(), 0u);

    double bestAsk;
    CHECK(book.getBestAsk(bestAsk));
    CHECK_EQ(bestAsk, 100.0);
}

TEST(exact_match_fully_fills_both_sides) {
    OrderBook book;
    book.addOrder(OrderFactory::createOrder(OrderKind::LIMIT, 1, 1, Side::SELL, 10, 100.0));
    auto trades = book.addOrder(OrderFactory::createOrder(OrderKind::LIMIT, 2, 2, Side::BUY, 10, 100.0));

    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].quantity, 10);
    CHECK_EQ(trades[0].price, 100.0);

    double dummy;
    CHECK(!book.getBestAsk(dummy)); // book should be empty on both sides now
    CHECK(!book.getBestBid(dummy));
}

TEST(partial_fill_leaves_remainder_resting) {
    OrderBook book;
    book.addOrder(OrderFactory::createOrder(OrderKind::LIMIT, 1, 1, Side::SELL, 20, 100.0));
    auto trades = book.addOrder(OrderFactory::createOrder(OrderKind::LIMIT, 2, 2, Side::BUY, 5, 100.0));

    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].quantity, 5);

    double bestAsk;
    CHECK(book.getBestAsk(bestAsk));
    CHECK_EQ(bestAsk, 100.0); // 15 shares still resting at 100
}

TEST(price_time_priority_fifo_within_a_price_level) {
    OrderBook book;
    // Two sell orders at the SAME price - order #1 arrived first, so it
    // must be the one that gets matched first (time priority).
    book.addOrder(OrderFactory::createOrder(OrderKind::LIMIT, 1, /*trader=*/10, Side::SELL, 5, 100.0));
    book.addOrder(OrderFactory::createOrder(OrderKind::LIMIT, 2, /*trader=*/20, Side::SELL, 5, 100.0));

    auto trades = book.addOrder(OrderFactory::createOrder(OrderKind::LIMIT, 3, 30, Side::BUY, 5, 100.0));

    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].sellOrderId, 1u); // must be the FIRST resting order, not #2
}

TEST(no_price_cross_means_no_match) {
    OrderBook book;
    book.addOrder(OrderFactory::createOrder(OrderKind::LIMIT, 1, 1, Side::SELL, 10, 105.0));
    // Buyer only willing to pay 100, ask is 105 - should NOT match.
    auto trades = book.addOrder(OrderFactory::createOrder(OrderKind::LIMIT, 2, 2, Side::BUY, 10, 100.0));

    CHECK_EQ(trades.size(), 0u);
    double bestBid, bestAsk;
    CHECK(book.getBestBid(bestBid));
    CHECK(book.getBestAsk(bestAsk));
    CHECK_EQ(bestBid, 100.0);
    CHECK_EQ(bestAsk, 105.0);
}

TEST(market_order_sweeps_available_liquidity) {
    OrderBook book;
    book.addOrder(OrderFactory::createOrder(OrderKind::LIMIT, 1, 1, Side::SELL, 10, 100.0));
    auto trades = book.addOrder(OrderFactory::createOrder(OrderKind::MARKET, 2, 2, Side::BUY, 10));

    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].quantity, 10);
}

TEST(market_order_with_no_liquidity_is_dropped_not_left_resting) {
    OrderBook book; // empty book, nothing to match against
    auto trades = book.addOrder(OrderFactory::createOrder(OrderKind::MARKET, 1, 1, Side::BUY, 10));

    CHECK_EQ(trades.size(), 0u);
    double dummy;
    CHECK(!book.getBestBid(dummy)); // must NOT be sitting in the book -
                                      // market orders never rest
}

TEST(cancel_removes_order_and_updates_best_price) {
    OrderBook book;
    book.addOrder(OrderFactory::createOrder(OrderKind::LIMIT, 1, 1, Side::SELL, 10, 100.0));
    book.addOrder(OrderFactory::createOrder(OrderKind::LIMIT, 2, 1, Side::SELL, 10, 101.0));

    CHECK(book.cancelOrder(1));

    double bestAsk;
    CHECK(book.getBestAsk(bestAsk));
    CHECK_EQ(bestAsk, 101.0); // #1 (the better price) is gone, #2 is now best
}

TEST(cancel_on_unknown_id_returns_false) {
    OrderBook book;
    CHECK(!book.cancelOrder(9999));
}

TEST(engine_stop_drains_submitted_orders) {
    Engine engine;
    engine.start();

    engine.submitOrder(OrderFactory::createOrder(
        OrderKind::LIMIT, 1, 1, Side::SELL, 10, 100.0));
    engine.submitOrder(OrderFactory::createOrder(
        OrderKind::LIMIT, 2, 2, Side::BUY, 10, 100.0));
    engine.stop();

    CHECK_EQ(engine.getTradeLog().size(), 1u);
    CHECK_EQ(engine.getTradeLog()[0].quantity, 10);
}

TEST(engine_can_restart_after_stop) {
    Engine engine;
    engine.start();
    engine.stop();

    engine.start();
    engine.submitOrder(OrderFactory::createOrder(
        OrderKind::LIMIT, 1, 1, Side::SELL, 3, 100.0));
    engine.submitOrder(OrderFactory::createOrder(
        OrderKind::LIMIT, 2, 2, Side::BUY, 3, 100.0));
    engine.stop();

    CHECK_EQ(engine.getTradeLog().size(), 1u);
}

TEST(invalid_orders_are_rejected) {
    bool quantityRejected = false;
    try {
        OrderFactory::createOrder(OrderKind::MARKET, 1, 1, Side::BUY, 0);
    } catch (const std::invalid_argument&) {
        quantityRejected = true;
    }
    CHECK(quantityRejected);

    bool priceRejected = false;
    try {
        OrderFactory::createOrder(OrderKind::LIMIT, 2, 1, Side::SELL, 1, -1.0);
    } catch (const std::invalid_argument&) {
        priceRejected = true;
    }
    CHECK(priceRejected);
}

TEST(duplicate_active_order_ids_are_rejected) {
    OrderBook book;
    book.addOrder(OrderFactory::createOrder(
        OrderKind::LIMIT, 1, 1, Side::SELL, 1, 100.0));

    bool rejected = false;
    try {
        book.addOrder(OrderFactory::createOrder(
            OrderKind::LIMIT, 1, 2, Side::BUY, 1, 100.0));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}

// ---------------------------------------------------------------------
// Crash-recovery tests. These use InMemoryJournal (a test double) so the exact
// append -> loadAll -> replay path used by PostgresJournal is exercised with
// zero external dependencies - i.e. recovery correctness is proven even on a
// machine with no database installed.
// ---------------------------------------------------------------------
TEST(recovery_rebuilds_resting_orders_trades_and_cancels) {
    InMemoryJournal journal;

    // --- First "process lifetime": run some commands, then go down. ---
    std::vector<Trade> beforeCrash;
    {
        Engine engine;
        engine.setJournal(&journal);
        engine.start();

        engine.submitOrder(OrderFactory::createOrder(OrderKind::LIMIT, 1, 1, Side::SELL, 10, 100.0));
        engine.submitOrder(OrderFactory::createOrder(OrderKind::LIMIT, 2, 2, Side::BUY,   4, 100.0)); // trades 4, leaves 6 resting
        engine.submitOrder(OrderFactory::createOrder(OrderKind::LIMIT, 3, 1, Side::SELL,  5, 101.0)); // rests at 101
        engine.cancelOrder(3);                                                                        // ...then cancelled
        engine.stop();

        beforeCrash = engine.getTradeLog();
    }
    CHECK_EQ(beforeCrash.size(), 1u);           // exactly one trade happened
    CHECK_EQ(beforeCrash[0].quantity, 4);
    CHECK(journal.size() == 4u);                // 3 submits + 1 cancel journaled

    // --- Second "process lifetime": fresh engine, SAME journal, recover. ---
    Engine recovered;
    recovered.setJournal(&journal);
    recovered.recover();

    // The recovered trade log must match exactly what happened before.
    std::vector<Trade> afterRecover = recovered.getTradeLog();
    CHECK_EQ(afterRecover.size(), beforeCrash.size());
    CHECK_EQ(afterRecover[0].buyOrderId, beforeCrash[0].buyOrderId);
    CHECK_EQ(afterRecover[0].sellOrderId, beforeCrash[0].sellOrderId);
    CHECK_EQ(afterRecover[0].quantity, beforeCrash[0].quantity);
    CHECK_EQ(afterRecover[0].price, beforeCrash[0].price);

    // Prove the BOOK state was rebuilt too: order #1 should still be resting
    // with 6 shares left at 100, and order #3 must be gone (it was cancelled).
    recovered.start();
    recovered.submitOrder(OrderFactory::createOrder(OrderKind::LIMIT, 4, 3, Side::BUY, 6, 100.0)); // should hit the resting 6 @100
    recovered.submitOrder(OrderFactory::createOrder(OrderKind::LIMIT, 5, 3, Side::BUY, 5, 101.0)); // nothing at 101 (cancelled) -> rests, no trade
    recovered.stop();

    std::vector<Trade> finalLog = recovered.getTradeLog();
    CHECK_EQ(finalLog.size(), 2u);               // the original trade + the new 6-share fill
    CHECK_EQ(finalLog[1].buyOrderId, 4u);
    CHECK_EQ(finalLog[1].sellOrderId, 1u);        // matched the recovered resting order
    CHECK_EQ(finalLog[1].quantity, 6);
    CHECK_EQ(finalLog[1].price, 100.0);
}

TEST(recovery_from_empty_journal_is_a_noop) {
    InMemoryJournal journal;
    Engine engine;
    engine.setJournal(&journal);
    engine.recover();                            // nothing to replay
    CHECK_EQ(engine.getTradeLog().size(), 0u);

    // Engine is still perfectly usable after an empty recovery.
    engine.start();
    engine.submitOrder(OrderFactory::createOrder(OrderKind::LIMIT, 1, 1, Side::SELL, 5, 100.0));
    engine.submitOrder(OrderFactory::createOrder(OrderKind::LIMIT, 2, 2, Side::BUY,  5, 100.0));
    engine.stop();
    CHECK_EQ(engine.getTradeLog().size(), 1u);
}

// The journal must record INPUT commands in order, with correct fields - this
// is what makes deterministic replay possible in the first place.
TEST(journal_records_commands_in_seq_order_with_correct_fields) {
    InMemoryJournal journal;
    {
        Engine engine;
        engine.setJournal(&journal);
        engine.start();
        engine.submitOrder(OrderFactory::createOrder(OrderKind::LIMIT, 100, 7, Side::SELL, 12, 99.5));
        engine.cancelOrder(100);
        engine.stop();
    }

    std::vector<JournalEvent> log = journal.loadAll();
    CHECK_EQ(log.size(), 2u);

    // Event 0: the SUBMIT, with every field preserved.
    CHECK_EQ(log[0].seq, 1u);                       // sequence numbers start at 1
    CHECK(log[0].type == CommandType::SUBMIT);
    CHECK(log[0].kind == OrderKind::LIMIT);
    CHECK(log[0].side == Side::SELL);
    CHECK_EQ(log[0].orderId, 100u);
    CHECK_EQ(log[0].traderId, 7u);
    CHECK_EQ(log[0].quantity, 12);
    CHECK_EQ(log[0].price, 99.5);

    // Event 1: the CANCEL, one higher in sequence, targeting the same id.
    CHECK_EQ(log[1].seq, 2u);
    CHECK(log[1].type == CommandType::CANCEL);
    CHECK_EQ(log[1].orderId, 100u);
}

// Replaying the SAME log into two independent engines must produce identical
// results - determinism is the whole premise of event-sourced recovery.
TEST(recovery_is_deterministic_across_engines) {
    InMemoryJournal journal;
    {
        Engine engine;
        engine.setJournal(&journal);
        engine.start();
        engine.submitOrder(OrderFactory::createOrder(OrderKind::LIMIT, 1, 1, Side::SELL, 10, 100.0));
        engine.submitOrder(OrderFactory::createOrder(OrderKind::LIMIT, 2, 2, Side::SELL,  5, 101.0));
        engine.submitOrder(OrderFactory::createOrder(OrderKind::LIMIT, 3, 3, Side::BUY,  12, 101.0)); // sweeps 10@100 then 2@101
        engine.stop();
    }

    Engine a, b;
    a.setJournal(&journal);
    b.setJournal(&journal);
    a.recover();
    b.recover();

    std::vector<Trade> la = a.getTradeLog();
    std::vector<Trade> lb = b.getTradeLog();
    CHECK_EQ(la.size(), lb.size());
    for (std::size_t i = 0; i < la.size(); ++i) {
        CHECK_EQ(la[i].buyOrderId, lb[i].buyOrderId);
        CHECK_EQ(la[i].sellOrderId, lb[i].sellOrderId);
        CHECK_EQ(la[i].quantity, lb[i].quantity);
        CHECK_EQ(la[i].price, lb[i].price);
    }
    // sanity: that crossing order really did generate two fills
    CHECK_EQ(la.size(), 2u);
}

// Market orders carry no price (stored as 0 in the journal). Recovery must
// rebuild them correctly via the factory's market path.
TEST(recovery_replays_market_orders) {
    InMemoryJournal journal;
    {
        Engine engine;
        engine.setJournal(&journal);
        engine.start();
        engine.submitOrder(OrderFactory::createOrder(OrderKind::LIMIT, 1, 1, Side::SELL, 10, 100.0));
        engine.submitOrder(OrderFactory::createOrder(OrderKind::MARKET, 2, 2, Side::BUY, 6)); // market buy
        engine.stop();
    }
    // The submitted market order was journaled as kind=MARKET, price=0.
    CHECK(journal.loadAll()[1].kind == OrderKind::MARKET);
    CHECK_EQ(journal.loadAll()[1].price, 0.0);

    Engine recovered;
    recovered.setJournal(&journal);
    recovered.recover();

    std::vector<Trade> log = recovered.getTradeLog();
    CHECK_EQ(log.size(), 1u);
    CHECK_EQ(log[0].buyOrderId, 2u);
    CHECK_EQ(log[0].sellOrderId, 1u);
    CHECK_EQ(log[0].quantity, 6);
    CHECK_EQ(log[0].price, 100.0); // market buy executes at the resting ask
}

int main() {
    return runAllTests();
}
