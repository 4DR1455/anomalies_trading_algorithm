/**
 * ============================================================================
 * Trading Execution Engine (The Hands)
 * ============================================================================
 * This is the main C++ service responsible for executing trades. It acts as a 
 * bridge between the Alpaca REST API and the Strategy Brain (OCaml). 
 * It manages state recovery on startup, guarantees execution via limit chasing, 
 * and handles network latency compensation.
 * ============================================================================
 */

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <sstream>
#include <nlohmann/json.hpp>

#include "Types.hpp"
#include "Utils.hpp"
#include "AlpacaAPI.hpp"
#include "BrainCommunicator.hpp"
#include "OrderManager.hpp"

using json = nlohmann::json;

int main() {
    std::cout << "[SYSTEM] Initializing Hands API Bridge..." << std::endl;
    
    // Load credentials from environment variables (Security best practice)
    std::string API_KEY, API_SECRET;
    if (const char* env_p = std::getenv("APCA_API_KEY_ID")) API_KEY = env_p;
    if (const char* env_p = std::getenv("APCA_API_SECRET_KEY")) API_SECRET = env_p;

    AlpacaAPI api(API_KEY, API_SECRET);
    BrainCommunicator brain(BRAIN_EXEC);
    OrderManager manager(api, brain);

    brain.start();

ini:
    std::cout << "[INIT] Synchronizing market state..." << std::endl;
    
    // 1. Clean up orphan buy orders.
    // This prevents the accidental execution of an interrupted 'chase' order 
    // if the bot previously crashed mid-transaction.
    try {
        std::string open_orders_json = api.get_open_orders();
        auto j_orders = json::parse(open_orders_json);
        bool has_deleted_buys = false;
        
        for (auto& order : j_orders) {
            if (order.contains("side") && order["side"].get<std::string>() == "buy") {
                std::string o_id = order["id"].get<std::string>();
                std::cout << "[INIT] Canceling orphan BUY order: " << o_id << std::endl;
                api.delete_order(o_id);
                has_deleted_buys = true;
            }
        }
        
        // If we canceled any buys, give Alpaca 2 seconds to release and refund the USD balance
        if (has_deleted_buys) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    } catch (const std::exception& e) {
        std::cerr << "[INIT ERROR] Failed to clean previous buys: " << e.what() << std::endl;
    }

    // Fetch initial market environment variables
    double f_price = api.get_price();
    double f_cash = api.get_cash();
    Position init_p = api.get_position();

    if (f_price > 0 && f_cash > -1) {
        std::cout << "[INIT] Base Price locked at: " << f_price << std::endl;
        std::string init_msg = "INFO " + std::to_string(f_cash) + ";" + std::to_string(f_price) + ";" + std::to_string(init_p.qty) + "\n";
        brain.send_message(init_msg); // Send initial state to OCaml Strategy Brain

        // State Recovery: If the bot holds an open position on boot
        if (init_p.qty > 0.01) {
            double g_id = init_p.avg_price / f_price;
            std::cout << "[INIT] Position Found: " << init_p.qty << " SOL. GridID: " << g_id << std::endl;
            brain.send_message("BOUGHT " + std::to_string(g_id) + "\n");
            
            double to_sell = init_p.qty_available;
            
            // If there are uncommitted shares, place a fresh sell order
            if (to_sell > 0.01) {
                double p_margin = get_profit_margin();
                double sell_target = init_p.avg_price * (1.0 + p_margin);
                
                std::string err;
                std::string s_id = api.send_limit_order("SELL", to_sell, sell_target, err);
                if (!s_id.empty()) {
                    std::cout << "[INIT] Placed maker sell order at " << sell_target << " for " << to_sell << " SOL" << std::endl;
                    manager.push_sell_order(s_id, to_sell, sell_target, g_id);
                    manager.set_shares_selling(manager.get_shares_selling() + to_sell);
                }
            } else {
                // If shares are tied up, recover existing sell orders from Alpaca
                std::cout << "[INIT] Active position. Recovering orphan orders from Alpaca..." << std::endl;
                manager.set_shares_selling(init_p.qty);
                
                try {
                    // 1. Fetch open orders from the broker
                    std::string open_orders_json = api.get_open_orders();
                    auto j_orders = json::parse(open_orders_json);
                    
                    // 2. Find and adopt existing sell orders
                    for (auto& order : j_orders) {
                        if (order.contains("side") && order["side"].get<std::string>() == "sell") {
                            std::string o_id = order["id"].get<std::string>();
                            double o_qty = std::stod(order["qty"].get<std::string>());
                            double o_price = std::stod(order["limit_price"].get<std::string>());
                            
                            std::cout << "[INIT] Order recovered: " << o_id << " (" << o_qty << " SOL @ " << o_price << " $)" << std::endl;
                            
                            // 3. Add them to the bot's internal tracking queue
                            manager.push_sell_order(o_id, o_qty, o_price, g_id);
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[INIT ERROR] Could not parse open orders from Alpaca: " << e.what() << std::endl;
                }
            }
        }
    } else {
        // Retry loop if API fails on boot
        std::this_thread::sleep_for(std::chrono::seconds(SLEEP_SECONDS));
        goto ini;
    }

    // ==========================================
    // MAIN EXECUTION LOOP
    // ==========================================
    while (true) {
        bool done = true;
        auto start_cycle = std::chrono::steady_clock::now();
        
        double price = api.get_price();
        double cash = api.get_cash();
        Position current_pos = api.get_position();
        double total_shares = current_pos.qty;

        if (price > 0 && cash > -1) {
            update_status_file(cash, total_shares, price); // Shared volume for Python Dashboard
            manager.review_sell_orders(cash, total_shares);

            std::cout << "[" << get_timestamp() << "] P: " << price << " | $: " << cash << " | S: " << total_shares << std::endl;

            // Feed current state to the Strategy Brain
            std::string info = "INFO " + std::to_string(cash) + ";" + std::to_string(price) + ";" + std::to_string(total_shares) + "\n";
            brain.send_message(info);

            // Read decision from IPC pipe (with 2-second timeout)
            bool success = false;
            std::string response = brain.read_message_with_timeout(2, success);

            if (!response.empty() && success) {
                std::stringstream ss(response);
                std::string type;
                double q; double ratio_sell;
                
                if (ss >> type >> q >> ratio_sell) {
                    // PROCESS BUY SIGNAL
                    if (type == "BUY" && q > 0) {
                        std::cout << "[BRAIN] BUY SIGNAL RECEIVED: " << q << " SOL" << std::endl;
                        
                        // Execute chase limit order to guarantee fill at best price
                        auto result = manager.execute_buy_chase(q, price);
                        
                        if (result.first > 0) {
                            double g_id = (result.second / f_price);
                            log_to_csv("BUY", result.second, result.first, cash - (result.first * result.second), total_shares + result.first);
                            
                            brain.send_message("BOUGHT " + std::to_string(g_id) + "\n");
                            
                            // 1. WAIT 2 SECONDS: Alpaca has internal latency when crediting crypto shares after a purchase
                            std::this_thread::sleep_for(std::chrono::seconds(2));
                            
                            // 2. Attempt to place the maker sell order for the acquired shares
                            double available_to_sell = result.first;
                            
                            if (available_to_sell > 0) {
                                double p_margin = get_profit_margin(); 
                                double min_target = result.second * (1.0 + p_margin);
                                double ema_target = (f_price * ratio_sell) * (1.0 - 0.0001);
                                
                                // Sell target is dynamically set to either the expected EMA reversion point or the minimum safety margin
                                double sell_target = std::max(min_target, ema_target);
                                
                                std::string err;
                                std::string s_id = api.send_limit_order("SELL", available_to_sell, sell_target, err);
                                
                                // LATENCY COMPENSATION: If Alpaca still reports "insufficient" balance, grant a 1-second grace period
                                if (s_id.empty() && err.find("insufficient") != std::string::npos) {
                                    std::cout << "[RETRY] Broker hasn't credited the balance yet. Waiting 1s..." << std::endl;
                                    std::this_thread::sleep_for(std::chrono::seconds(1));
                                    s_id = api.send_limit_order("SELL", available_to_sell, sell_target, err);
                                }

                                if (!s_id.empty()) {
                                    std::cout << "[SELL] Maker order placed at " << sell_target << " for " << available_to_sell << " SOL" << std::endl;
                                    manager.push_sell_order(s_id, available_to_sell, sell_target, g_id);
                                    manager.set_shares_selling(manager.get_shares_selling() + available_to_sell);
                                } else {
                                    std::cerr << "[SYSTEM ERROR] Critical failure placing sell order: " << err << std::endl;
                                }
                            }
                        } else {
                            // If the chase timeouts or fails, rollback the internal state of the Brain
                            std::cout << "[SYSTEM] Chase failed. Alerting Brain to rollback state." << std::endl;
                            brain.send_message("ROLLBACK\n");
                            done = false; // Fast-track the next cycle
                        }
                    }
                }
            }
        }

        // Maintain a strict operational tick rate by calculating sleep delta
        if (done && cash > -1) {
            auto end_cycle = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_cycle - start_cycle).count();
            long remaining_sleep = (SLEEP_SECONDS * 1000) - elapsed;
            if (remaining_sleep > 0) std::this_thread::sleep_for(std::chrono::milliseconds(remaining_sleep));
        }
    }
    return 0;
}
