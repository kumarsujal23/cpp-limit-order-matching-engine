#include "Order.h"
#include "OrderFactory.h"
#include "OrderBook.h"
#include "Stock.h"
#include "Trader.h"
#include <iostream>

void printTrade(const Trade& t) {
    std::cout << "  TRADE: buy#" << t.buyOrderId << " x sell#" << t.sellOrderId
              << " | qty=" << t.quantity << " @ price=" << t.price << "\n";
}

int main() {
    OrderBook book;

    std::cout << "=== Scenario 1: resting sell order, no match yet ===\n";
    // Alice (trader 1) offers to SELL 10 shares at 2498. No buyers yet,
    // so this simply rests in the book - addOrder returns an empty vector
    // of trades.
    auto t1 = book.addOrder(OrderFactory::createOrder(
        OrderKind::LIMIT, 1, /*traderId=*/1, Side::SELL, 10, 2498.0));
    std::cout << "Trades generated: " << t1.size() << "\n";
    book.printBook();

    std::cout << "\n=== Scenario 2: incoming buy fully matches it ===\n";
    // Bob (trader 2) wants to BUY 10 shares, willing to pay up to 2500 -
    // that's >= the resting ask of 2498, so this matches immediately.
    auto t2 = book.addOrder(OrderFactory::createOrder(
        OrderKind::LIMIT, 2, /*traderId=*/2, Side::BUY, 10, 2500.0));
    std::cout << "Trades generated: " << t2.size() << "\n";
    for (const auto& t : t2) printTrade(t);
    book.printBook();

    std::cout << "\n=== Scenario 3: partial fill ===\n";
    // Alice sells 20 more at 2497.
    book.addOrder(OrderFactory::createOrder(
        OrderKind::LIMIT, 3, 1, Side::SELL, 20, 2497.0));
    // Bob only wants 5 of them - order #4 fully fills, leaving 15 resting
    // from order #3.
    auto t3 = book.addOrder(OrderFactory::createOrder(
        OrderKind::LIMIT, 4, 2, Side::BUY, 5, 2497.0));
    std::cout << "Trades generated: " << t3.size() << "\n";
    for (const auto& t : t3) printTrade(t);
    book.printBook();

    std::cout << "\n=== Scenario 4: market order sweeps remaining liquidity ===\n";
    // A market buy for 15 shares - no price limit, just fill at whatever's
    // available. Should consume the remaining 15 shares of order #3.
    auto t4 = book.addOrder(OrderFactory::createOrder(
        OrderKind::MARKET, 5, 2, Side::BUY, 15));
    std::cout << "Trades generated: " << t4.size() << "\n";
    for (const auto& t : t4) printTrade(t);
    book.printBook();

    std::cout << "\n=== Scenario 5: cancel a resting order ===\n";
    book.addOrder(OrderFactory::createOrder(
        OrderKind::LIMIT, 6, 1, Side::SELL, 8, 2505.0));
    book.printBook();
    bool cancelled = book.cancelOrder(6);
    std::cout << "Cancel order #6 succeeded: " << std::boolalpha << cancelled << "\n";
    book.printBook();

    return 0;
}
