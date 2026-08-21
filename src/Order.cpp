#include "Order.h"
#include <algorithm> // std::max
#include <stdexcept>

// Member initializer list (the `: id_(id), traderId_(traderId), ...` part):
// this INITIALIZES members directly as the object is constructed, rather than
// default-constructing them and then assigning in the body. For simple types
// like int it barely matters, but it's the correct habit for const members,
// reference members, and members without a default constructor - so learn it
// now rather than retrofit it later.
Order::Order(OrderId id, TraderId traderId, Side side, int quantity)
    : id_(id),
      traderId_(traderId),
      side_(side),
      quantity_(quantity),
      remainingQty_(quantity),
      timestamp_(std::chrono::steady_clock::now()),
      status_(OrderStatus::NEW)
{
    if (quantity <= 0) {
        throw std::invalid_argument("Order quantity must be positive");
    }
}

OrderId   Order::getId() const              { return id_; }
TraderId  Order::getTraderId() const        { return traderId_; }
Side      Order::getSide() const            { return side_; }
int       Order::getQuantity() const        { return quantity_; }
int       Order::getRemainingQuantity() const { return remainingQty_; }
Timestamp Order::getTimestamp() const       { return timestamp_; }
OrderStatus Order::getStatus() const        { return status_; }

void Order::reduceQuantity(int filledQty) {
    // std::max guards against filledQty being larger than what's left,
    // which would otherwise make remainingQty_ negative - a bug that's
    // easy to introduce later once the matching loop gets more complex.
    remainingQty_ = std::max(0, remainingQty_ - filledQty);
}

void Order::setStatus(OrderStatus status) {
    status_ = status;
}
