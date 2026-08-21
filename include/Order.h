#pragma once
#include "Types.h"
#include <string>

// Order is an ABSTRACT base class: it declares the interface every kind of order
// must support, but you can never create an `Order` object directly - only
// `MarketOrder` or `LimitOrder`. A class becomes abstract in C++ the moment it has
// at least one "pure virtual" function (the `= 0` you'll see below).
//
// Why abstract base + subclasses instead of one class with an `if (isMarket) ...`
// flag everywhere? Because polymorphism lets the OrderBook (which we build next
// week) call order->getPrice() without caring whether it's holding a MarketOrder
// or LimitOrder - each subclass provides its own correct behavior. This is the
// Open/Closed Principle: OrderBook is "closed" for modification but "open" for
// new order types, since adding a StopOrder later won't touch OrderBook's code.
class Order {
public:
    Order(OrderId id, TraderId traderId, Side side, int quantity);

    // A VIRTUAL DESTRUCTOR is mandatory whenever a class is meant to be
    // subclassed and deleted through a base pointer (which we will do:
    // std::unique_ptr<Order> holding a LimitOrder). Without `virtual` here,
    // deleting through a base pointer only runs ~Order(), leaking whatever
    // the derived class allocated. This is a classic C++ interview gotcha.
    virtual ~Order() = default;

    // Pure virtual functions: `= 0` means "no implementation here, every
    // concrete subclass MUST provide one." This is what makes Order abstract.
    virtual OrderKind getOrderKind() const = 0;
    virtual double getPrice() const = 0;   // LimitOrder returns its limit price;
                                            // MarketOrder's meaning we'll discuss
                                            // when we implement it below.

    // Ordinary (non-virtual) accessors - same for every order type, so no
    // need for subclasses to override these. `const` after the signature
    // means "this function promises not to modify the object" - lets you
    // call it on a `const Order&` too, and documents intent.
    OrderId getId() const;
    TraderId getTraderId() const;
    Side getSide() const;
    int getQuantity() const;          // original requested quantity
    int getRemainingQuantity() const; // still unfilled - shrinks as we match
    Timestamp getTimestamp() const;
    OrderStatus getStatus() const;

    // Mutators used by the matching engine (next week) to record fills.
    void reduceQuantity(int filledQty);
    void setStatus(OrderStatus status);

protected:
    // `protected`, not `private`: subclasses (MarketOrder, LimitOrder) can
    // touch these directly; outside code cannot. Encapsulation, but not
    // walled off from the family.
    OrderId id_;
    TraderId traderId_;
    Side side_;
    int quantity_;
    int remainingQty_;
    Timestamp timestamp_;
    OrderStatus status_;
};
