#pragma once
#include <list>
#include "Types.hpp"
#include "AlpacaAPI.hpp"
#include "BrainCommunicator.hpp"

class OrderManager {
public:
    OrderManager(AlpacaAPI& api, BrainCommunicator& brain);
    
    void review_sell_orders(double cash, double shares);
    void review_buy_orders(double cash, double shares);
    
    std::pair<double, double> execute_maker_buy_chase(double qty, double price);
    std::pair<double, double> execute_maker_sell_chase(double qty, double price);

    void clear_queues();
    void push_sell_order(const std::string& id, double qty, double price, double grid_id);
    void push_buy_order(const std::string& id, double qty, double price, double grid_id);

private:
    AlpacaAPI& api;
    BrainCommunicator& brain;
    std::list<ActiveOrder> sell_orders_queue;
    std::list<ActiveOrder> buy_orders_queue;
};
