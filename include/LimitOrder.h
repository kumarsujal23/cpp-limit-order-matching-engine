#pragma once
#include "Order.h"

// `class LimitOrder : public Order` - this is INHERITANCE. `public` inheritance
// means "a LimitOrder IS-A Order" - anywhere code expects an Order (or a pointer/
// reference to one), a LimitOrder can be used instead. This is the substitution
// that makes polymorphism work.
class LimitOrder : public Order {
public:
    LimitOrder(OrderId id, TraderId traderId, Side side, int quantity, double price);

    // `override` is not strictly required by the compiler, but ALWAYS write it.
    // It tells the compiler "I intend to override a virtual function from the
    // base class" - if you typo the signature (wrong const-ness, wrong types),
    // the compiler errors immediately instead of silently creating an unrelated
    // new function that never gets called. Saves hours of confused debugging.
    OrderKind getOrderKind() const override;
    double getPrice() const override;

private:
    double price_;
};
