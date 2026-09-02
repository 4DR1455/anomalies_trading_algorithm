#include "OrderManager.hpp"
#include "Utils.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

OrderManager::OrderManager(AlpacaAPI& api, BrainCommunicator& brain) : api(api), brain(brain) {}

void OrderManager::push_sell_order(const std::string& id, double qty, double price, double grid_id) {
    sell_orders_queue.push_back({id, "SELL", qty, price, grid_id});
}

void OrderManager::set_shares_selling(double shares) { shares_selling = shares; }
double OrderManager::get_shares_selling() { return shares_selling; }

void OrderManager::review_sell_orders(double cash, double shares) {
    auto it = sell_orders_queue.begin();
    while (it != sell_orders_queue.end()) {
        std::string status = "unknown";
        double filled_p = 0.0;
        
        try {
            std::string res = api.get_order_status(it->id);
            auto j = json::parse(res);
            if (j.contains("status")) status = j["status"].get<std::string>();
            if (j.contains("filled_avg_price") && !j["filled_avg_price"].is_null()) {
                filled_p = std::stod(j["filled_avg_price"].get<std::string>());
            }
        } catch (...) {}

        if (status == "filled") {
            std::cout << "[SELL] Sortida amb èxit per grid: " << it->grid_id << " @ " << filled_p << std::endl;
            log_to_csv("SELL", filled_p, it->qty, cash + (it->qty * filled_p), shares - it->qty);
            
            std::string msg = "SOLD " + std::to_string(it->grid_id) + "\n";
            brain.send_message(msg);
            
            Position pos = api.get_position();
            shares_selling = pos.qty - pos.qty_available;
            it = sell_orders_queue.erase(it);
        } else if (status == "canceled" || status == "expired") {
            std::cout << "[SELL] Ordre " << it->id << " expirada. Resubmetent..." << std::endl;
            std::string err;
            std::string s_id = api.send_limit_order("SELL", it->qty, it->price, err);
            if (!s_id.empty()) {
                sell_orders_queue.push_back({s_id, "SELL", it->qty, it->price, it->grid_id});
            }
            it = sell_orders_queue.erase(it);
        } else {
            ++it;
        }
    }
}

std::pair<double, double> OrderManager::execute_buy_chase(double qty, double price) {
    std::string err;
    std::string order_id = api.send_limit_order("BUY", qty, price, err);
    if (order_id.empty()) return {0.0, 0.0};

    std::cout << "[CHASE] Cercant compra de " << qty << " SOL..." << std::endl;
    for (int i = 0; i < CHASE_WAIT_SECONDS; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        try {
            std::string res = api.get_order_status(order_id);
            auto j = json::parse(res);
            if (j.contains("status") && j["status"].get<std::string>() == "filled") {
                double filled_p = std::stod(j["filled_avg_price"].get<std::string>());
                return {qty, filled_p};
            }
        } catch (...) {}
    }

    std::cout << "[CHASE] Temps d'espera esgotat. Cancel·lant." << std::endl;
    api.delete_order(order_id);
    return {0.0, 0.0};
}