
#ifndef ORDERMANAGER_HPP
#define ORDERMANAGER_HPP

#include "../vda5050/vda5050.hpp"

class OrderManager {
public:
    OrderManager(vda5050::State& State, vda5050::Order& Order);

    void update();
private:

    vda5050::State& State_;
    vda5050::Order Order_;
};



#endif //ORDERMANAGER_HPP
