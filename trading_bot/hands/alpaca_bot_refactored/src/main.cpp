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
    std::cout << "[SYSTEM] Inicialitzant Hands API Bridge..." << std::endl;
    
    std::string API_KEY, API_SECRET;
    if (const char* env_p = std::getenv("APCA_API_KEY_ID")) API_KEY = env_p;
    if (const char* env_p = std::getenv("APCA_API_SECRET_KEY")) API_SECRET = env_p;

    AlpacaAPI api(API_KEY, API_SECRET);
    BrainCommunicator brain(BRAIN_EXEC);
    OrderManager manager(api, brain);

    brain.start();

ini:
    std::cout << "[INIT] Sincronitzant estat del mercat..." << std::endl;
    
    // 1. Neteja d'ordres de compra òrfenes (Evita execucions d'un 'chase' interromput)
    try {
        std::string open_orders_json = api.get_open_orders();
        auto j_orders = json::parse(open_orders_json);
        bool has_deleted_buys = false;
        
        for (auto& order : j_orders) {
            if (order.contains("side") && order["side"].get<std::string>() == "buy") {
                std::string o_id = order["id"].get<std::string>();
                std::cout << "[INIT] Cancel·lant ordre de COMPRA òrfena: " << o_id << std::endl;
                api.delete_order(o_id);
                has_deleted_buys = true;
            }
        }
        
        // Si hem cancel·lat alguna compra, donem 2 segons a Alpaca perquè ens retorni els dòlars al balanç
        if (has_deleted_buys) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    } catch (const std::exception& e) {
        std::cerr << "[INIT ERROR] Fallada al netejar compres prèvies: " << e.what() << std::endl;
    }

    double f_price = api.get_price();
    double f_cash = api.get_cash();
    Position init_p = api.get_position();

    if (f_price > 0 && f_cash > -1) {
        std::cout << "[INIT] Preu Base bloquejat a: " << f_price << std::endl;
        std::string init_msg = "INFO " + std::to_string(f_cash) + ";" + std::to_string(f_price) + ";" + std::to_string(init_p.qty) + "\n";
        brain.send_message(init_msg);

        if (init_p.qty > 0.01) {
            double g_id = init_p.avg_price / f_price;
            std::cout << "[INIT] Posició Trobada: " << init_p.qty << " SOL. GridID: " << g_id << std::endl;
            brain.send_message("BOUGHT " + std::to_string(g_id) + "\n");
            
            double to_sell = init_p.qty_available;
            if (to_sell > 0.01) {
                double p_margin = get_profit_margin();
                double sell_target = init_p.avg_price * (1.0 + p_margin);
                
                std::string err;
                std::string s_id = api.send_limit_order("SELL", to_sell, sell_target, err);
                if (!s_id.empty()) {
                    std::cout << "[INIT] Ordre de venda a " << sell_target << " per " << to_sell << " SOL" << std::endl;
                    manager.push_sell_order(s_id, to_sell, sell_target, g_id);
                    manager.set_shares_selling(manager.get_shares_selling() + to_sell);
                }
            } else {
                std::cout << "[INIT] Posició activa. Recuperant ordres òrfenes d'Alpaca..." << std::endl;
                manager.set_shares_selling(init_p.qty);
                
                try {
                    // 1. Demanem les ordres obertes al broker
                    std::string open_orders_json = api.get_open_orders();
                    auto j_orders = json::parse(open_orders_json);
                    
                    // 2. Busquem i adoptem les ordres de venda
                    for (auto& order : j_orders) {
                        if (order.contains("side") && order["side"].get<std::string>() == "sell") {
                            std::string o_id = order["id"].get<std::string>();
                            double o_qty = std::stod(order["qty"].get<std::string>());
                            double o_price = std::stod(order["limit_price"].get<std::string>());
                            
                            std::cout << "[INIT] Ordre recuperada: " << o_id << " (" << o_qty << " SOL @ " << o_price << " $)" << std::endl;
                            
                            // 3. L'afegim a la cua de control del bot
                            manager.push_sell_order(o_id, o_qty, o_price, g_id);
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[INIT ERROR] No s'han pogut llegir les ordres d'Alpaca: " << e.what() << std::endl;
                }
            }
        }
    } else {
        std::this_thread::sleep_for(std::chrono::seconds(SLEEP_SECONDS));
        goto ini;
    }

    while (true) {
        bool done = true;
        auto start_cycle = std::chrono::steady_clock::now();
        
        double price = api.get_price();
        double cash = api.get_cash();
        Position current_pos = api.get_position();
        double total_shares = current_pos.qty;

        if (price > 0 && cash > -1) {
            update_status_file(cash, total_shares, price);
            manager.review_sell_orders(cash, total_shares);

            std::cout << "[" << get_timestamp() << "] P: " << price << " | $: " << cash << " | S: " << total_shares << std::endl;

            std::string info = "INFO " + std::to_string(cash) + ";" + std::to_string(price) + ";" + std::to_string(total_shares) + "\n";
            brain.send_message(info);

            bool success = false;
            std::string response = brain.read_message_with_timeout(2, success);

            if (!response.empty() && success) {
                std::stringstream ss(response);
                std::string type;
                double q; double ratio_sell;
                
                if (ss >> type >> q >> ratio_sell) {
                    if (type == "BUY" && q > 0) {
                        std::cout << "[BRAIN] SENYAL DE COMPRA: " << q << " SOL" << std::endl;
                        auto result = manager.execute_buy_chase(q, price);
                        
                        if (result.first > 0) {
                            double g_id = (result.second / f_price);
                            log_to_csv("BUY", result.second, result.first, cash - (result.first * result.second), total_shares + result.first);
                            
                            brain.send_message("BOUGHT " + std::to_string(g_id) + "\n");
                            
                            // 1. ESPEREM 2 SEGONS: Alpaca triga una mica a posar les accions al nostre balanç després de comprar
                            std::this_thread::sleep_for(std::chrono::seconds(2));
                            
                            // 2. Intentem vendre
                            double available_to_sell = result.first;
                            
                            if (available_to_sell > 0) {
                                double p_margin = get_profit_margin(); 
                                double min_target = result.second * (1.0 + p_margin);
                                double ema_target = (f_price * ratio_sell) * (1.0 - 0.0001);
                                double sell_target = std::max(min_target, ema_target);
                                
                                std::string err;
                                std::string s_id = api.send_limit_order("SELL", available_to_sell, sell_target, err);
                                
                                // Si per culpa del lag Alpaca encara diu "insufficient", donem 1 segon més de marge
                                if (s_id.empty() && err.find("insufficient") != std::string::npos) {
                                    std::cout << "[RETRY] Alpaca encara no ha acreditat el saldo. Esperant 1s més..." << std::endl;
                                    std::this_thread::sleep_for(std::chrono::seconds(1));
                                    s_id = api.send_limit_order("SELL", available_to_sell, sell_target, err);
                                }

                                if (!s_id.empty()) {
                                    std::cout << "[SELL] Esquer col·locat a " << sell_target << " per " << available_to_sell << " SOL" << std::endl;
                                    manager.push_sell_order(s_id, available_to_sell, sell_target, g_id);
                                    manager.set_shares_selling(manager.get_shares_selling() + available_to_sell);
                                } else {
                                    std::cerr << "[SYSTEM ERROR] Fallada crítica col·locant ordre de venda: " << err << std::endl;
                                }
                            }
                        } else {
                            std::cout << "[SYSTEM] Fallada al chase. Avisant al Brain." << std::endl;
                            brain.send_message("ROLLBACK\n");
                            done = false;
                        }
                    }
                }
            }
        }

        if (done && cash > -1) {
            auto end_cycle = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_cycle - start_cycle).count();
            long remaining_sleep = (SLEEP_SECONDS * 1000) - elapsed;
            if (remaining_sleep > 0) std::this_thread::sleep_for(std::chrono::milliseconds(remaining_sleep));
        }
    }
    return 0;
}