#pragma once
#include <string>

// A plain, non-polymorphic class - no inheritance needed here, so no virtual
// destructor, no base class. Not everything needs to be part of a hierarchy;
// forcing inheritance where it isn't needed is a common over-engineering habit
// worth avoiding (an interviewer will notice if every class inherits from
// something for no reason).
class Stock {
public:
    Stock(std::string name, double price);

    const std::string& getName() const; // return by const ref: avoids copying
                                          // the string just to read it
    double getPrice() const;
    void setPrice(double price);

private:
    std::string name_;
    double price_;
};
