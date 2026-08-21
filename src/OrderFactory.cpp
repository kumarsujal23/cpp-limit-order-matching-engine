#include "OrderFactory.h"
#include "MarketOrder.h"
#include "LimitOrder.h"

std::unique_ptr<Order> OrderFactory::createOrder(
    OrderKind kind, OrderId id, TraderId traderId, Side side, int quantity, double price)
{
    switch (kind) {
        case OrderKind::MARKET:
            // std::make_unique constructs the object AND wraps it in a
            // unique_ptr in one step. Prefer this over
            // `std::unique_ptr<Order>(new MarketOrder(...))` because it's
            // exception-safe (no window where `new` succeeds but the wrap
            // into unique_ptr hasn't happened yet) and it's simply shorter.
            return std::make_unique<MarketOrder>(id, traderId, side, quantity);
        case OrderKind::LIMIT:
            return std::make_unique<LimitOrder>(id, traderId, side, quantity, price);
    }
    // Every enum case handled above, but compilers can't always prove that -
    // this keeps the function well-defined (and silences "no return"
    // warnings) if OrderKind ever grows a new case someone forgets to handle.
    throw std::logic_error("Unknown OrderKind");
}
