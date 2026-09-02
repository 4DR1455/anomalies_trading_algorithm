#pragma once
#include <list>
#include "Types.hpp"
#include "AlpacaAPI.hpp"
#include "BrainCommunicator.hpp"

class OrderManager {
public:
    OrderManager(AlpacaAPI& api, BrainCommunicator& brain);
    
    void review_sell_orders(double cash, double shares);
    std::pair<double, double> execute_buy_chase(double qty, double price);
    
    void push_sell_order(const std::string& id, double qty, double price, double grid_id);
    void set_shares_selling(double shares);
    double get_shares_selling();

private:
    AlpacaAPI& api;
    BrainCommunicator& brain;
    std::list<ActiveOrder> sell_orders_queue;
    double shares_selling = 0.0;
};