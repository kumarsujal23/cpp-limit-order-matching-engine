#include "MarketOrder.h"

MarketOrder::MarketOrder(OrderId id, TraderId traderId, Side side, int quantity)
    : Order(id, traderId, side, quantity)
{}

OrderKind MarketOrder::getOrderKind() const { return OrderKind::MARKET; }

double MarketOrder::getPrice() const {
    throw std::logic_error("MarketOrder has no fixed price - check getOrderKind() first");
}
