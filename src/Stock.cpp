#include "Stock.h"
#include <utility> // std::move

// `std::string name` taken BY VALUE in the constructor, then std::move'd into
// the member - this is the modern idiom for constructors that store a copy of
// their argument. It lets the compiler optimize away extra copies whether the
// caller passes an lvalue or a temporary, without you writing two overloads.
Stock::Stock(std::string name, double price)
    : name_(std::move(name)), price_(price)
{}

const std::string& Stock::getName() const { return name_; }
double Stock::getPrice() const { return price_; }
void Stock::setPrice(double price) { price_ = price; }
