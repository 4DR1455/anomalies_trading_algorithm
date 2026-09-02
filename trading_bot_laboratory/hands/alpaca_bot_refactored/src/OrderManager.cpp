#include "OrderManager.hpp"
#include "Utils.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

OrderManager::OrderManager(AlpacaAPI& api, BrainCommunicator& brain) : api(api), brain(brain) {}

void OrderManager::clear_queues() {
    sell_orders_queue.clear();
    buy_orders_queue.clear();
}

void OrderManager::push_sell_order(const std::string& id, double qty, double price, double grid_id) {
    sell_orders_queue.push_back({id, qty, price, grid_id});
}

void OrderManager::push_buy_order(const std::string& id, double qty, double price, double grid_id) {
    buy_orders_queue.push_back({id, qty, price, grid_id});
}

void OrderManager::review_sell_orders(double cash, double shares) {
    std::string open_orders_res = api.get_open_orders();
    std::vector<std::string> open_order_ids;
    try {
        auto j_arr = json::parse(open_orders_res);
        if (j_arr.is_array()) {
            for (auto& order : j_arr) {
                if (order.contains("id")) open_order_ids.push_back(order["id"].get<std::string>());
            }
        } else return;
    } catch (...) { return; }

    auto it = sell_orders_queue.begin();
    while (it != sell_orders_queue.end()) {
        if (it->id.empty()) {
            std::string err_msg;
            std::string new_id = api.send_limit_order("SELL", it->qty, it->price, err_msg);
            
            if (new_id.empty() && err_msg.find("insufficient") != std::string::npos) {
                size_t pos = err_msg.find("available: ");
                if (pos != std::string::npos) {
                    size_t end_pos = err_msg.find(")", pos + 11);
                    if (end_pos != std::string::npos) {
                        std::string true_qty = err_msg.substr(pos + 11, end_pos - (pos + 11));
                        std::string d_err;
                        new_id = api.send_limit_order_raw("SELL", true_qty, it->price, d_err);
                        if (!new_id.empty()) it->qty = std::stod(true_qty);
                    }
                }
            }
            if (new_id.empty() && err_msg.find("wash trade") != std::string::npos) {
                it->price += 0.01; 
                ++it; continue;
            }
            if (!new_id.empty()) { it->id = new_id; ++it; continue; }
            ++it; continue; 
        }

        if (std::find(open_order_ids.begin(), open_order_ids.end(), it->id) != open_order_ids.end()) { ++it; continue; }

        std::string res = api.get_order_status(it->id);
        std::string status = "unknown"; double filled_p = 0.0; double filled_q = 0.0; bool is_dead = false;
        try { 
            auto j = json::parse(res);
            if (j.contains("status")) status = j["status"].get<std::string>();
            if (j.contains("code") && j["code"] == 40410000) is_dead = true;
            if (j.contains("message") && j["message"].get<std::string>().find("not found") != std::string::npos) is_dead = true;
            if (j.contains("filled_avg_price") && !j["filled_avg_price"].is_null()) filled_p = std::stod(j["filled_avg_price"].get<std::string>());
            if (j.contains("filled_qty") && !j["filled_qty"].is_null()) filled_q = std::stod(j["filled_qty"].get<std::string>());
        } catch (...) {}

        if (status == "filled") {
            std::cout << "[VALL SORTIDA] ✅ Tancament executat: Venuts " << it->qty << " SOL a " << filled_p << " $" << std::endl;
            log_to_csv("VALL_OUT (Sell)", filled_p, it->qty, cash + (it->qty * filled_p), shares - it->qty);
            std::string msg = "SOLD " + std::to_string(it->grid_id) + "\n";
            brain.send_message(msg);
            it = sell_orders_queue.erase(it);
        } else if (status == "canceled" || status == "expired" || is_dead) {
            double remaining_qty = it->qty - filled_q;
            if (filled_q > 0.0 && !is_dead) {
                std::cout << "[VALL SORTIDA] ⚠️ Tancament parcial: " << filled_q << " SOL a " << filled_p << " $" << std::endl;
                log_to_csv("VALL_OUT (Partial)", filled_p, filled_q, cash + (filled_q * filled_p), shares - filled_q);
                std::string msg = "SOLD " + std::to_string(it->grid_id) + "\n";
                brain.send_message(msg);
            }
            if (remaining_qty > 0.0001) {
                std::string err;
                std::string new_id = api.send_limit_order("SELL", remaining_qty, it->price, err);
                it->id = new_id; it->qty = remaining_qty; ++it; 
            } else { it = sell_orders_queue.erase(it); }
        } else { ++it; }
    }
}

void OrderManager::review_buy_orders(double cash, double shares) {
    std::string open_orders_res = api.get_open_orders();
    std::vector<std::string> open_order_ids;
    try {
        auto j_arr = json::parse(open_orders_res);
        if (j_arr.is_array()) {
            for (auto& order : j_arr) {
                if (order.contains("id")) open_order_ids.push_back(order["id"].get<std::string>());
            }
        } else return;
    } catch (...) { return; }

    auto it = buy_orders_queue.begin();
    while (it != buy_orders_queue.end()) {
        if (it->id.empty()) {
            std::string err_msg;
            std::string new_id = api.send_limit_order("BUY", it->qty, it->price, err_msg);
            
            if (new_id.empty() && err_msg.find("insufficient") != std::string::npos) {
                double safe_cash = api.get_cash();
                if (safe_cash > 1.0) {
                    double new_qty = (safe_cash * 0.99) / it->price;
                    std::string d_err;
                    new_id = api.send_limit_order_raw("BUY", format_qty(new_qty), it->price, d_err);
                    if (!new_id.empty()) it->qty = new_qty;
                }
            }
            if (new_id.empty() && err_msg.find("wash trade") != std::string::npos) {
                it->price -= 0.01; 
                ++it; continue;
            }
            
            if (!new_id.empty()) { it->id = new_id; ++it; continue; }
            ++it; continue; 
        }

        if (std::find(open_order_ids.begin(), open_order_ids.end(), it->id) != open_order_ids.end()) { ++it; continue; }

        std::string res = api.get_order_status(it->id);
        std::string status = "unknown"; double filled_p = 0.0; double filled_q = 0.0; bool is_dead = false;
        try { 
            auto j = json::parse(res);
            if (j.contains("status")) status = j["status"].get<std::string>();
            if (j.contains("code") && j["code"] == 40410000) is_dead = true;
            if (j.contains("message") && j["message"].get<std::string>().find("not found") != std::string::npos) is_dead = true;
            if (j.contains("filled_avg_price") && !j["filled_avg_price"].is_null()) filled_p = std::stod(j["filled_avg_price"].get<std::string>());
            if (j.contains("filled_qty") && !j["filled_qty"].is_null()) filled_q = std::stod(j["filled_qty"].get<std::string>());
        } catch (...) {}

        if (status == "filled") {
            std::cout << "[PIC RECOMPRA] ✅ Ordre executada: Comprats " << it->qty << " SOL a " << filled_p << " $" << std::endl;
            log_to_csv("PIC_OUT (Buy)", filled_p, it->qty, cash - (it->qty * filled_p), shares + it->qty);
            std::string msg = "BOUGHT " + std::to_string(it->grid_id) + "\n";
            brain.send_message(msg);
            it = buy_orders_queue.erase(it);
        } else if (status == "canceled" || status == "expired" || is_dead) {
            double remaining_qty = it->qty - filled_q;
            if (filled_q > 0.0 && !is_dead) {
                std::cout << "[PIC RECOMPRA] ⚠️ Compra parcial: " << filled_q << " SOL a " << filled_p << " $" << std::endl;
                log_to_csv("PIC_OUT (Partial)", filled_p, filled_q, cash - (filled_q * filled_p), shares + filled_q);
                std::string msg = "BOUGHT " + std::to_string(it->grid_id) + "\n";
                brain.send_message(msg);
            }
            if (remaining_qty > 0.0001) {
                std::string err;
                std::string new_id = api.send_limit_order("BUY", remaining_qty, it->price, err);
                it->id = new_id; it->qty = remaining_qty; ++it; 
            } else { it = buy_orders_queue.erase(it); }
        } else { ++it; }
    }
}

std::pair<double, double> OrderManager::execute_maker_buy_chase(double qty, double price) {
    std::string err;
    std::string order_id = api.send_limit_order("BUY", qty, price, err);
    
    if (order_id.empty() && err.find("insufficient") != std::string::npos) {
        double safe_cash = api.get_cash();
        if (safe_cash > 1.0) {
            qty = (safe_cash * 0.99) / price;
            order_id = api.send_limit_order("BUY", qty, price, err);
        }
    }
    
    if (order_id.empty()) return {0.0, 0.0};
    
    bool fully_filled = false;
    for (int i = 0; i < CHASE_WAIT_SECONDS; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::string res = api.get_order_status(order_id);
        try {
            auto j = json::parse(res);
            if (j["status"].get<std::string>() == "filled") { fully_filled = true; break; }
        } catch (...) {}
    }
    
    if (!fully_filled) {
        api.delete_order(order_id);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    double filled_q = 0.0; double filled_p = 0.0;
    std::string final_res = api.get_order_status(order_id);
    try {
        auto j = json::parse(final_res);
        if (j.contains("filled_qty")) filled_q = std::stod(j["filled_qty"].get<std::string>());
        if (j.contains("filled_avg_price") && !j["filled_avg_price"].is_null()) {
            filled_p = std::stod(j["filled_avg_price"].get<std::string>());
        }
    } catch (...) {}

    if (filled_q > 0.0) {
        std::cout << "[VALL ENTRADA] 🛒 Inversió confirmada: " << filled_q << " SOL a " << filled_p << " $" << std::endl;
    }
    return {filled_q, filled_p};
}

std::pair<double, double> OrderManager::execute_maker_sell_chase(double qty, double price) {
    std::string err;
    std::string order_id = api.send_limit_order("SELL", qty, price, err);
    
    if (order_id.empty() && err.find("insufficient") != std::string::npos) {
        size_t pos = err.find("available: ");
        if (pos != std::string::npos) {
            size_t end_pos = err.find(")", pos + 11);
            if (end_pos != std::string::npos) {
                std::string true_qty = err.substr(pos + 11, end_pos - (pos + 11));
                qty = std::stod(true_qty);
                order_id = api.send_limit_order_raw("SELL", true_qty, price, err);
            }
        }
    }
    
    if (order_id.empty()) return {0.0, 0.0};
    
    bool fully_filled = false;
    for (int i = 0; i < CHASE_WAIT_SECONDS; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::string res = api.get_order_status(order_id);
        try {
            auto j = json::parse(res);
            if (j["status"].get<std::string>() == "filled") { fully_filled = true; break; }
        } catch (...) {}
    }
    
    if (!fully_filled) {
        api.delete_order(order_id);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    double filled_q = 0.0; double filled_p = 0.0;
    std::string final_res = api.get_order_status(order_id);
    try {
        auto j = json::parse(final_res);
        if (j.contains("filled_qty")) filled_q = std::stod(j["filled_qty"].get<std::string>());
        if (j.contains("filled_avg_price") && !j["filled_avg_price"].is_null()) {
            filled_p = std::stod(j["filled_avg_price"].get<std::string>());
        }
    } catch (...) {}

    if (filled_q > 0.0) {
        std::cout << "[PIC ENTRADA] 📉 Sortida confirmada: " << filled_q << " SOL a " << filled_p << " $" << std::endl;
    }
    return {filled_q, filled_p};
}
