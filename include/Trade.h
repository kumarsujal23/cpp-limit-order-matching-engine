#pragma once
#include "Types.h"

// A Trade is the OUTPUT of matching: proof that a buy order and a sell order
// were paired up. It's a simple "data-only" struct (all public, no invariants
// to protect) - not every type needs to be a full encapsulated class. Using a
// plain struct here signals to a reader "this is just a value, not a thing
// with behavior," which is honest about what it actually is.
struct Trade {
    OrderId buyOrderId;
    OrderId sellOrderId;
    double price;
    int quantity;
    Timestamp timestamp;
};
