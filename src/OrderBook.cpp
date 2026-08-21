#include "OrderBook.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

std::vector<Trade> OrderBook::addOrder(std::unique_ptr<Order> order) {
    if (!order) {
        throw std::invalid_argument("Cannot add a null order");
    }

    OrderId id = order->getId();
    if (ownedOrders_.find(id) != ownedOrders_.end()) {
        throw std::invalid_argument("Order ID is already active");
    }

    // Grab a raw pointer BEFORE we move the unique_ptr away. This raw
    // pointer is safe to use afterwards precisely because of the
    // unordered_map guarantee explained in the header: as long as we don't
    // erase THIS entry, the object it points to doesn't move or get freed,
    // even if the map rehashes internally when other entries are added.
    Order* raw = order.get();
    ownedOrders_[id] = std::move(order);

    std::vector<Trade> trades = matchIncoming(raw);

    if (raw->getRemainingQuantity() == 0) {
        // Fully filled - nothing left to do but mark it and drop our
        // ownership (its data already lives on inside the Trade records).
        raw->setStatus(OrderStatus::FILLED);
        ownedOrders_.erase(id);
    } else if (raw->getOrderKind() == OrderKind::LIMIT) {
        // Still has quantity left AND it's a limit order -> it rests in
        // the book waiting for a future match.
        raw->setStatus(raw->getRemainingQuantity() == raw->getQuantity()
                            ? OrderStatus::OPEN
                            : OrderStatus::PARTIALLY_FILLED);
        restInBook(raw);
    } else {
        // Market order with leftover quantity and no more liquidity to
        // match against - real exchanges simply drop the unfilled portion
        // rather than letting a market order "wait" (that would defeat the
        // point of a market order, which is "fill me now").
        raw->setStatus(OrderStatus::CANCELLED);
        ownedOrders_.erase(id);
    }

    return trades;
}

std::vector<Trade> OrderBook::matchIncoming(Order* incoming) {
    std::vector<Trade> trades;

    // ---- Incoming BUY matches against the ASK side ----
    if (incoming->getSide() == Side::BUY) {
        while (incoming->getRemainingQuantity() > 0 && !asks_.empty()) {
            auto bestLevelIt = asks_.begin();      // lowest ask price = map's first entry
            double bestAskPrice = bestLevelIt->first;

            // A market order matches at any price. A limit buy only
            // matches if it's willing to pay at least the best ask price.
            bool priceOk = (incoming->getOrderKind() == OrderKind::MARKET)
                            || (incoming->getPrice() >= bestAskPrice);
            if (!priceOk) break; // best ask is too expensive - stop matching

            std::list<Order*>& level = bestLevelIt->second;
            Order* resting = level.front(); // oldest order at this price = time priority

            int tradeQty = std::min(incoming->getRemainingQuantity(),
                                     resting->getRemainingQuantity());

            trades.push_back(Trade{
                /*buyOrderId=*/  incoming->getId(),
                /*sellOrderId=*/ resting->getId(),
                /*price=*/       bestAskPrice, // trades execute at the RESTING order's
                                                // price, not the aggressor's - standard
                                                // exchange convention (price improvement
                                                // goes to whoever was waiting).
                /*quantity=*/    tradeQty,
                /*timestamp=*/   std::chrono::steady_clock::now()
            });

            incoming->reduceQuantity(tradeQty);
            resting->reduceQuantity(tradeQty);

            if (resting->getRemainingQuantity() == 0) {
                resting->setStatus(OrderStatus::FILLED);
                OrderId restingId = resting->getId();
                level.pop_front();
                orderLocation_.erase(restingId);
                ownedOrders_.erase(restingId); // done with this order entirely
                if (level.empty()) {
                    asks_.erase(bestLevelIt); // no orders left at this price - drop the level
                }
            } else {
                resting->setStatus(OrderStatus::PARTIALLY_FILLED);
            }
        }
    }
    // ---- Incoming SELL matches against the BID side (mirror image) ----
    else {
        while (incoming->getRemainingQuantity() > 0 && !bids_.empty()) {
            auto bestLevelIt = bids_.begin();      // highest bid price (map uses std::greater)
            double bestBidPrice = bestLevelIt->first;

            bool priceOk = (incoming->getOrderKind() == OrderKind::MARKET)
                            || (incoming->getPrice() <= bestBidPrice);
            if (!priceOk) break;

            std::list<Order*>& level = bestLevelIt->second;
            Order* resting = level.front();

            int tradeQty = std::min(incoming->getRemainingQuantity(),
                                     resting->getRemainingQuantity());

            trades.push_back(Trade{
                /*buyOrderId=*/  resting->getId(),
                /*sellOrderId=*/ incoming->getId(),
                /*price=*/       bestBidPrice,
                /*quantity=*/    tradeQty,
                /*timestamp=*/   std::chrono::steady_clock::now()
            });

            incoming->reduceQuantity(tradeQty);
            resting->reduceQuantity(tradeQty);

            if (resting->getRemainingQuantity() == 0) {
                resting->setStatus(OrderStatus::FILLED);
                OrderId restingId = resting->getId();
                level.pop_front();
                orderLocation_.erase(restingId);
                ownedOrders_.erase(restingId);
                if (level.empty()) {
                    bids_.erase(bestLevelIt);
                }
            } else {
                resting->setStatus(OrderStatus::PARTIALLY_FILLED);
            }
        }
    }

    return trades;
}

void OrderBook::restInBook(Order* incoming) {
    double price = incoming->getPrice(); // safe: only called for LIMIT orders
    OrderId id = incoming->getId();

    if (incoming->getSide() == Side::BUY) {
        std::list<Order*>& level = bids_[price]; // creates the level with an
                                                   // empty list if it doesn't exist yet
        level.push_back(incoming);
        // std::prev(level.end()) = iterator to the element we JUST pushed
        // (the last one). We stash it so cancelOrder/matching can erase
        // this exact node in O(1) later without searching.
        orderLocation_[id] = OrderLocation{Side::BUY, price, std::prev(level.end())};
    } else {
        std::list<Order*>& level = asks_[price];
        level.push_back(incoming);
        orderLocation_[id] = OrderLocation{Side::SELL, price, std::prev(level.end())};
    }
}

bool OrderBook::cancelOrder(OrderId id) {
    auto locIt = orderLocation_.find(id);
    if (locIt == orderLocation_.end()) {
        return false; // not resting in the book (never existed, already filled, etc.)
    }

    const OrderLocation& loc = locIt->second;

    if (loc.side == Side::BUY) {
        auto levelIt = bids_.find(loc.price);
        levelIt->second.erase(loc.iterator); // O(1): list erase via iterator
        if (levelIt->second.empty()) bids_.erase(levelIt);
    } else {
        auto levelIt = asks_.find(loc.price);
        levelIt->second.erase(loc.iterator);
        if (levelIt->second.empty()) asks_.erase(levelIt);
    }

    orderLocation_.erase(locIt);

    auto ownedIt = ownedOrders_.find(id);
    if (ownedIt != ownedOrders_.end()) {
        ownedIt->second->setStatus(OrderStatus::CANCELLED);
        ownedOrders_.erase(ownedIt);
    }
    return true;
}

bool OrderBook::getBestBid(double& outPrice) const {
    if (bids_.empty()) return false;
    outPrice = bids_.begin()->first;
    return true;
}

bool OrderBook::getBestAsk(double& outPrice) const {
    if (asks_.empty()) return false;
    outPrice = asks_.begin()->first;
    return true;
}

void OrderBook::printBook() const {
    std::cout << "----- ASKS (best at bottom) -----\n";
    for (auto it = asks_.rbegin(); it != asks_.rend(); ++it) {
        std::cout << "  " << it->first << " x " << it->second.size() << " order(s)\n";
    }
    std::cout << "----------------------------------\n";
    for (const auto& [price, level] : bids_) {
        std::cout << "  " << price << " x " << level.size() << " order(s)\n";
    }
    std::cout << "----- BIDS (best at top) -----\n";
}
