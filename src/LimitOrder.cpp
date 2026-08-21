#include "LimitOrder.h"
#include <cmath>
#include <stdexcept>

LimitOrder::LimitOrder(OrderId id, TraderId traderId, Side side, int quantity, double price)
    : Order(id, traderId, side, quantity), // must call base constructor explicitly
      price_(price)
    {
      if (!std::isfinite(price) || price <= 0.0) {
        throw std::invalid_argument("Limit order price must be finite and positive");
      }
    }

OrderKind LimitOrder::getOrderKind() const { return OrderKind::LIMIT; }
double LimitOrder::getPrice() const { return price_; }
