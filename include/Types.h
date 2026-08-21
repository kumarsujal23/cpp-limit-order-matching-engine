#pragma once
// #pragma once tells the compiler "only include this file once per translation unit,
// even if it's #included from multiple places". Prevents duplicate-definition errors.
// (The old-school alternative is #ifndef/#define include guards - same effect, uglier.)

#include <cstdint>
#include <chrono>

// enum class (a "scoped enum") instead of plain enum:
//   - Side::BUY, not just BUY floating in the global namespace polluting everything else.
//   - Can't accidentally compare a Side to an OrderKind, even though both are ints underneath.
// Your original diagram used plain `enum OrderType`, which doesn't give you this safety.
enum class Side {
    BUY,
    SELL
};

enum class OrderKind {
    MARKET,
    LIMIT
};

enum class OrderStatus {
    NEW,             // just created, not yet in the book
    OPEN,            // resting in the book, unfilled or partially filled
    PARTIALLY_FILLED,
    FILLED,
    CANCELLED
};

// Type aliases: give a plain type a meaningful name. Costs nothing at runtime,
// makes function signatures self-documenting (OrderId vs some anonymous int).
using OrderId  = std::uint64_t;
using TraderId = std::uint64_t;

// We use std::chrono instead of old C time_t (which your diagram used) because
// chrono gives us type-safe, high-resolution timestamps - useful once we need
// microsecond-level ordering for time priority.
using Timestamp = std::chrono::steady_clock::time_point;
