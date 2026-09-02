/**
 * ============================================================================
 * Trading Execution Engine - Main Loop (Laboratory Version)
 * ============================================================================
 * Coordinates the dual-direction "Peaks and Valleys" (Pics i Valls) strategy.
 * Listens for commands from the OCaml Brain via IPC pipes and dispatches 
 * complex chase actions (buying dips or shorting peaks) via the OrderManager.
 * ============================================================================
 */

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <sstream>

#include "Types.hpp"
#include "Utils.hpp"
#include "AlpacaAPI.hpp"
#include "BrainCommunicator.hpp"
#include "OrderManager.hpp"

int main() {
    std::string API_KEY, API_SECRET;
    if (const char* env_p = std::getenv("APCA_API_KEY_ID")) API_KEY = env_p;
    if (const char* env_p = std::getenv("APCA_API_SECRET_KEY")) API_SECRET = env_p;

    AlpacaAPI api(API_KEY, API_SECRET);
    BrainCommunicator brain(BRAIN_EXEC);
    OrderManager manager(api, brain);

    brain.start();

ini:
    std::cout << "[INIT] Unifying and initializing environment..." << std::endl;
    api.delete_all_orders(); // Clear slate on startup
    
    // Ensure broker has flushed all pending open orders
    for (int i = 0; i < 15; i++) {
        std::string check_res = api.get_open_orders();
        if (check_res == "[]" || check_res.empty()) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    double f_price_init = 0.0;
    double f_price = api.get_price();
    double f_cash = api.get_cash();
    Position init_p = api.get_position();
    
    if (f_price > 0 && f_cash > -1) {
        f_price_init = f_price;
        std::string init_msg = "INFO " + std::to_string(f_cash) + ";" + std::to_string(f_price) + ";" + std::to_string(init_p.qty) + "\n";
        brain.send_message(init_msg);
    } else {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        goto ini;
    }

    // ==========================================
    // MAIN EXECUTION TICK LOOP
    // ==========================================
    while (true) {
        double price = api.get_price();
        double cash = api.get_cash();
        Position pos = api.get_position();

        if (price > 0 && cash > -1) {
            update_status_file(cash, pos.qty, price);

            std::cout << "[" << get_timestamp() << "] 🟢 ACTIVE -> Price: " << price << " $ | Cash: " << cash << " $ | SOL: " << pos.qty << std::endl;

            // Review active background queues for both sides of the grid
            manager.review_sell_orders(cash, pos.qty);
            manager.review_buy_orders(cash, pos.qty);

            std::string info = "INFO " + std::to_string(cash) + ";" + std::to_string(price) + ";" + std::to_string(pos.qty) + "\n";
            brain.send_message(info);

            bool success = false;
            std::string response = brain.read_message_with_timeout(2, success);

            if (!response.empty() && success) {
                std::stringstream ss(response);
                std::string type; 
                
                if (ss >> type) {
                    // HANDLE SYSTEM COMMANDS (FREEZE / UNFREEZE)
                    if (type == "CMD") {
                        std::string cmd;
                        if (ss >> cmd) {
                            if (cmd == "FREEZE") {
                                api.delete_all_orders();
                                manager.clear_queues();
                                
                                Position p = api.get_position();
                                if (p.qty_available > 0.001) {
                                    std::string e;
                                    api.send_limit_order_raw("SELL", p.raw_qty_available, price * 0.98, e); 
                                }
                            } else if (cmd == "UNFREEZE") {
                                api.delete_all_orders();
                                manager.clear_queues();
                                std::this_thread::sleep_for(std::chrono::seconds(3));
                                
                                double current_cash = api.get_cash();
                                Position p = api.get_position();
                                double current_equity = current_cash + (p.qty * price);
                                
                                double target_qty = (current_equity * 0.5) / price;
                                std::string e;
                                
                                if (target_qty > p.qty + 0.001) {
                                    double buy_qty = target_qty - p.qty;
                                    api.send_limit_order_raw("BUY", format_qty(buy_qty), price * 1.02, e);
                                } else if (target_qty < p.qty - 0.001) {
                                    double sell_qty = p.qty - target_qty;
                                    api.send_limit_order_raw("SELL", format_qty(sell_qty), price * 0.98, e);
                                }
                            }
                        }
                    } 
                    // HANDLE VALLEY (VALL) PROTOCOL: Buy dips, target higher exit
                    else if (type == "VALL") {
                        double q, entry_r, exit_r;
                        if (ss >> q >> entry_r >> exit_r) {
                            double entry_p = (entry_r < 2.0) ? f_price_init * entry_r : entry_r;
                            double exit_p = (exit_r < 2.0) ? f_price_init * exit_r : exit_r;
                            
                            api.delete_all_orders();
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));

                            auto res = manager.execute_maker_buy_chase(q, entry_p);
                            
                            if (res.first > 0) {
                                log_to_csv("VALL_IN (Buy)", res.second, res.first, cash - (res.first * res.second), pos.qty + res.first);
                                brain.send_message("BOUGHT 0\n");
                                
                                double qty_to_sell = res.first;
                                std::string err_msg;
                                std::string s_id = api.send_limit_order("SELL", qty_to_sell, exit_p, err_msg);
                                
                                // Self-healing fallback for insufficient position balances during sell placement
                                if (s_id.empty() && err_msg.find("insufficient") != std::string::npos) {
                                    size_t s_pos = err_msg.find("available: ");
                                    if (s_pos != std::string::npos) {
                                        size_t end_pos = err_msg.find(")", s_pos + 11);
                                        if (end_pos != std::string::npos) {
                                            std::string true_qty = err_msg.substr(s_pos + 11, end_pos - (s_pos + 11));
                                            std::string d_err;
                                            s_id = api.send_limit_order_raw("SELL", true_qty, exit_p, d_err);
                                            if (!s_id.empty()) qty_to_sell = std::stod(true_qty);
                                        }
                                    }
                                }

                                if (s_id.empty()) manager.push_sell_order("", qty_to_sell, exit_p, 0.0);
                                else manager.push_sell_order(s_id, qty_to_sell, exit_p, 0.0);
                            }
                        }
                    } 
                    // HANDLE PEAK (PIC) PROTOCOL: Sell peaks, target lower buyback
                    else if (type == "PIC") {
                        double q, entry_r, exit_r;
                        if (ss >> q >> entry_r >> exit_r) {
                            double entry_p = (entry_r < 2.0) ? f_price_init * entry_r : entry_r;
                            double exit_p = (exit_r < 2.0) ? f_price_init * exit_r : exit_r;

                            api.delete_all_orders();
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));

                            auto res = manager.execute_maker_sell_chase(q, entry_p);
                            
                            if (res.first > 0) {
                                log_to_csv("PIC_IN (Sell)", res.second, res.first, cash + (res.first * res.second), pos.qty - res.first);
                                brain.send_message("SOLD 0\n");
                                
                                double qty_to_buy = (res.first * res.second) / exit_p;
                                std::string err_msg;
                                std::string b_id = api.send_limit_order("BUY", qty_to_buy, exit_p, err_msg);
                                
                                // Self-healing fallback for insufficient cash balances during buyback placement
                                if (b_id.empty() && err_msg.find("insufficient") != std::string::npos) {
                                    double safe_cash = api.get_cash();
                                    if (safe_cash > 1.0) {
                                        qty_to_buy = (safe_cash * 0.99) / exit_p;
                                        std::string d_err;
                                        b_id = api.send_limit_order_raw("BUY", format_qty(qty_to_buy), exit_p, d_err);
                                    }
                                }

                                if (b_id.empty()) manager.push_buy_order("", qty_to_buy, exit_p, 0.0);
                                else manager.push_buy_order(b_id, qty_to_buy, exit_p, 0.0);
                            }
                        }
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(SLEEP_SECONDS));
    }
    return 0;
}
