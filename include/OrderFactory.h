#pragma once
#include "Order.h"
#include <memory>

// FACTORY PATTERN: instead of callers doing
//     new LimitOrder(...) or new MarketOrder(...)
// directly (which forces every call site to know about every concrete order
// type), they call OrderFactory::createOrder(...) and get back an Order they
// can use polymorphically. This matters more once we add StopOrder or
// IcebergOrder later - only the factory needs to change, not every call site.
//
// std::unique_ptr<Order> as the return type says, in the type system itself:
// "you now own this order exclusively; when it goes out of scope, it's freed
// automatically." No manual delete, no risk of a double-free. This is called
// RAII (Resource Acquisition Is Initialization) - arguably THE core C++ idiom,
// and a phrase worth being able to explain fluently in an interview.
class OrderFactory {
public:
    // price is only meaningful for LIMIT orders; ignored for MARKET.
    static std::unique_ptr<Order> createOrder(
        OrderKind kind,
        OrderId id,
        TraderId traderId,
        Side side,
        int quantity,
        double price = 0.0
    );
};
