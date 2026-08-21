#pragma once
#include "Types.h"
#include <string>
#include <unordered_map>

// std::unordered_map<std::string, int>: a HASH MAP from stock name -> quantity
// held. O(1) average lookup/insert, unlike std::map's O(log n) - we use
// unordered_map here because we don't need the holdings sorted by name, we
// just need fast "how many shares of X do I hold" lookups. (Contrast this
// with the order book itself next week, where sorted order IS the point -
// that's why the book will use std::map, not unordered_map. Choosing between
// these two is a very common interview question.)
class Trader {
public:
    Trader(TraderId id, std::string name);

    TraderId getId() const;
    const std::string& getName() const;

    int getHolding(const std::string& stockName) const;
    void addHolding(const std::string& stockName, int quantity);
    // removeHolding returns false if the trader doesn't have enough shares -
    // callers must check this rather than assume it always succeeds.
    bool removeHolding(const std::string& stockName, int quantity);

private:
    TraderId id_;
    std::string name_;
    std::unordered_map<std::string, int> holdings_;
};
