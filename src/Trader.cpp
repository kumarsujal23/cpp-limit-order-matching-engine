#include "Trader.h"
#include <utility>

Trader::Trader(TraderId id, std::string name)
    : id_(id), name_(std::move(name))
{}

TraderId Trader::getId() const { return id_; }
const std::string& Trader::getName() const { return name_; }

int Trader::getHolding(const std::string& stockName) const {
    // .find() instead of operator[] on a CONST map: operator[] would insert
    // a default-constructed entry if the key is missing, which we can't do
    // on a const object anyway (and shouldn't do even on a non-const one,
    // just to read a value). .find() returns an iterator; compare it to
    // .end() to check for "not found".
    auto it = holdings_.find(stockName);
    return (it != holdings_.end()) ? it->second : 0;
}

void Trader::addHolding(const std::string& stockName, int quantity) {
    holdings_[stockName] += quantity; // operator[] default-inits to 0 if new
}

bool Trader::removeHolding(const std::string& stockName, int quantity) {
    auto it = holdings_.find(stockName);
    if (it == holdings_.end() || it->second < quantity) {
        return false; // don't own enough - reject, don't go negative
    }
    it->second -= quantity;
    return true;
}
