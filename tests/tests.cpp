#include "TestFramework.h"
#include "Engine.h"
#include "OrderBook.h"
#include "OrderFactory.h"

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

int main() {
    return runAllTests();
}
