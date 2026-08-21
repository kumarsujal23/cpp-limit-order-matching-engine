#pragma once
#include "Order.h"
#include <stdexcept>

// A MarketOrder has no price - "fill me now at whatever the market offers."
// Design question worth thinking about: what should getPrice() do here, given
// the base class demands *some* implementation?
//
// We throw. Alternative designs (return 0, return a sentinel like -1) are
// tempting but dangerous: a bug elsewhere that forgets to special-case market
// orders would silently treat "price 0" as a real price and misbehave instead
// of crashing loudly during testing. Failing fast is usually the safer choice
// for a code path that should genuinely never be hit - next week, when we
// write the matching engine, it will check getOrderKind() BEFORE ever calling
// getPrice(), so this exception is a safety net that should never fire in
// correct code, not a normal-path return value.
class MarketOrder : public Order {
public:
    MarketOrder(OrderId id, TraderId traderId, Side side, int quantity);

    OrderKind getOrderKind() const override;
    double getPrice() const override; // throws std::logic_error if called
};
