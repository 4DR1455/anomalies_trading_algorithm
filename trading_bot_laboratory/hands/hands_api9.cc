#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <list>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cmath> 
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <algorithm>

using json = nlohmann::json;

const std::string BRAIN_EXEC = "./brain";
const std::string SYMBOL = "SOL/USD";         
const std::string ASSET_SYMBOL = "SOLUSD";    
const int SLEEP_SECONDS = 30; 
const int CHASE_WAIT_SECONDS = 20; 

const std::string BASE_URL = "https://paper-api.alpaca.markets";
const std::string DATA_URL = "https://data.alpaca.markets/v1beta3/crypto/us/latest/quotes";

std::string API_KEY;
std::string API_SECRET;
double f_price_init = 0.0;
double DEFAULT_PROFIT_MARGIN = 0.004; // Nomes per al rescat en el INIT

struct ActiveOrder {
    std::string id;
    double qty;
    double price;
    double grid_id; 
};

struct Position { 
    double qty = 0.0; 
    double qty_available = 0.0; 
    double avg_price = 0.0; 
    std::string raw_qty_available = ""; 
};

std::list<ActiveOrder> sell_orders_queue;
std::list<ActiveOrder> buy_orders_queue; 
bool intime;

std::string format_qty(double val) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(4) << val;
    return ss.str();
}

std::string format_price(double val) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2) << val;
    return ss.str();
}

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string http_request(const std::string& url, const std::string& method, const std::string& body = "") {
    CURL* curl = curl_easy_init();
    std::string readBuffer;
    if(curl) {
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, ("APCA-API-KEY-ID: " + API_KEY).c_str());
        headers = curl_slist_append(headers, ("APCA-API-SECRET-KEY: " + API_SECRET).c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); 
        if (method == "POST") { 
            curl_easy_setopt(curl, CURLOPT_POST, 1L); 
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str()); 
        }
        else if (method == "DELETE") { 
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE"); 
        }
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    }
    return readBuffer;
}

std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm* local_time = std::localtime(&now_time);
    std::stringstream ss;
    ss << std::put_time(local_time, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void log_to_csv(std::string type, double price, double qty, double cash, double shares_held) {
    std::ofstream file("data.csv", std::ios::app);
    if (file.is_open()) {
        file << get_timestamp() << "," << type << "," 
             << std::fixed << std::setprecision(4) << price << "," 
             << std::setprecision(4) << qty << "," 
             << std::setprecision(2) << cash << "," 
             << std::setprecision(4) << shares_held << "\n";
        file.close();
    }
}

void update_status_file(double cash, double shares, double price) {
    std::ofstream file("status.json", std::ios::trunc);
    if (file.is_open()) {
        double invested = shares * price;
        json j;
        j["equity"] = cash + invested;
        j["cash"] = cash;
        j["invested"] = invested;
        j["price"] = price;
        j["shares"] = shares;
        j["timestamp"] = get_timestamp();
        file << j.dump(4);
        file.close();
    }
}

double get_price() {
    std::string response = http_request(DATA_URL + "?symbols=" + SYMBOL, "GET");
    try {
        auto j = json::parse(response);
        if (j.contains("quotes") && j["quotes"].contains(SYMBOL)) {
            return (j["quotes"][SYMBOL]["bp"].get<double>() + j["quotes"][SYMBOL]["ap"].get<double>()) / 2.0;
        }
    } catch (...) {}
    return 0.0;
}

double get_cash() {
    std::string response = http_request(BASE_URL + "/v2/account", "GET");
    try { 
        auto j = json::parse(response);
        return std::stod(j["cash"].get<std::string>()); 
    } catch (...) { return -1; }
}

Position get_position() {
    Position pos;
    std::string response = http_request(BASE_URL + "/v2/positions/" + ASSET_SYMBOL, "GET");
    try { 
        auto j = json::parse(response);
        if (j.contains("qty") && !j["qty"].is_null()) pos.qty = std::stod(j["qty"].get<std::string>());
        if (j.contains("avg_entry_price") && !j["avg_entry_price"].is_null()) {
            std::string p_str = j["avg_entry_price"].get<std::string>();
            if (!p_str.empty()) pos.avg_price = std::stod(p_str);
        }
        if (j.contains("qty_available") && !j["qty_available"].is_null()) {
            pos.raw_qty_available = j["qty_available"].get<std::string>();
            if (!pos.raw_qty_available.empty()) pos.qty_available = std::stod(pos.raw_qty_available);
        } else if (j.contains("qty") && !j["qty"].is_null()) {
            pos.raw_qty_available = j["qty"].get<std::string>();
            pos.qty_available = pos.qty;
        }
    } catch (...) {}
    return pos;
}

std::string send_limit_order_raw(const std::string& side, const std::string& qty_str, double price, std::string& out_error) {
    json o; o["symbol"] = SYMBOL; o["qty"] = qty_str; o["side"] = (side == "BUY" ? "buy" : "sell");
    o["type"] = "limit"; o["limit_price"] = format_price(price); o["time_in_force"] = "gtc";
    std::string res = http_request(BASE_URL + "/v2/orders", "POST", o.dump());
    try { 
        auto j = json::parse(res); 
        if (j.contains("id")) return j["id"].get<std::string>(); 
        if (j.contains("message")) {
            out_error = j["message"].get<std::string>();
            std::cerr << "[ALPACA ERROR] " << side << ": " << out_error << std::endl;
        }
    } catch (...) {}
    return "";
}

std::string send_limit_order(const std::string& side, double qty, double price, std::string& out_error) {
    return send_limit_order_raw(side, format_qty(qty), price, out_error);
}

void review_sell_orders(int pipe_to_brain, double cash, double shares) {
    std::string open_orders_res = http_request(BASE_URL + "/v2/orders?status=open", "GET");
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
            std::string new_id = send_limit_order("SELL", it->qty, it->price, err_msg);
            
            if (new_id.empty() && err_msg.find("insufficient") != std::string::npos) {
                size_t pos = err_msg.find("available: ");
                if (pos != std::string::npos) {
                    size_t end_pos = err_msg.find(")", pos + 11);
                    if (end_pos != std::string::npos) {
                        std::string true_qty = err_msg.substr(pos + 11, end_pos - (pos + 11));
                        std::string d_err;
                        new_id = send_limit_order_raw("SELL", true_qty, it->price, d_err);
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

        std::string res = http_request(BASE_URL + "/v2/orders/" + it->id, "GET");
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
            write(pipe_to_brain, msg.c_str(), msg.size());
            it = sell_orders_queue.erase(it);
        } else if (status == "canceled" || status == "expired" || is_dead) {
            double remaining_qty = it->qty - filled_q;
            if (filled_q > 0.0 && !is_dead) {
                std::cout << "[VALL SORTIDA] ⚠️ Tancament parcial: " << filled_q << " SOL a " << filled_p << " $" << std::endl;
                log_to_csv("VALL_OUT (Partial)", filled_p, filled_q, cash + (filled_q * filled_p), shares - filled_q);
                std::string msg = "SOLD " + std::to_string(it->grid_id) + "\n";
                write(pipe_to_brain, msg.c_str(), msg.size());
            }
            if (remaining_qty > 0.0001) {
                std::string err;
                std::string new_id = send_limit_order("SELL", remaining_qty, it->price, err);
                it->id = new_id; it->qty = remaining_qty; ++it; 
            } else { it = sell_orders_queue.erase(it); }
        } else { ++it; }
    }
}

void review_buy_orders(int pipe_to_brain, double cash, double shares) {
    std::string open_orders_res = http_request(BASE_URL + "/v2/orders?status=open", "GET");
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
            std::string new_id = send_limit_order("BUY", it->qty, it->price, err_msg);
            
            if (new_id.empty() && err_msg.find("insufficient") != std::string::npos) {
                double safe_cash = get_cash();
                if (safe_cash > 1.0) {
                    double new_qty = (safe_cash * 0.99) / it->price;
                    std::string d_err;
                    new_id = send_limit_order_raw("BUY", format_qty(new_qty), it->price, d_err);
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

        std::string res = http_request(BASE_URL + "/v2/orders/" + it->id, "GET");
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
            write(pipe_to_brain, msg.c_str(), msg.size());
            it = buy_orders_queue.erase(it);
        } else if (status == "canceled" || status == "expired" || is_dead) {
            double remaining_qty = it->qty - filled_q;
            if (filled_q > 0.0 && !is_dead) {
                std::cout << "[PIC RECOMPRA] ⚠️ Compra parcial: " << filled_q << " SOL a " << filled_p << " $" << std::endl;
                log_to_csv("PIC_OUT (Partial)", filled_p, filled_q, cash - (filled_q * filled_p), shares + filled_q);
                std::string msg = "BOUGHT " + std::to_string(it->grid_id) + "\n";
                write(pipe_to_brain, msg.c_str(), msg.size());
            }
            if (remaining_qty > 0.0001) {
                std::string err;
                std::string new_id = send_limit_order("BUY", remaining_qty, it->price, err);
                it->id = new_id; it->qty = remaining_qty; ++it; 
            } else { it = buy_orders_queue.erase(it); }
        } else { ++it; }
    }
}

std::pair<double, double> execute_maker_buy_chase(double qty, double price) {
    std::string err;
    std::string order_id = send_limit_order("BUY", qty, price, err);
    
    if (order_id.empty() && err.find("insufficient") != std::string::npos) {
        double safe_cash = get_cash();
        if (safe_cash > 1.0) {
            qty = (safe_cash * 0.99) / price;
            order_id = send_limit_order("BUY", qty, price, err);
        }
    }
    
    if (order_id.empty()) return {0.0, 0.0};
    
    bool fully_filled = false;
    for (int i = 0; i < CHASE_WAIT_SECONDS; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::string res = http_request(BASE_URL + "/v2/orders/" + order_id, "GET");
        try {
            auto j = json::parse(res);
            if (j["status"].get<std::string>() == "filled") { fully_filled = true; break; }
        } catch (...) {}
    }
    
    if (!fully_filled) {
        http_request(BASE_URL + "/v2/orders/" + order_id, "DELETE");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    double filled_q = 0.0; double filled_p = 0.0;
    std::string final_res = http_request(BASE_URL + "/v2/orders/" + order_id, "GET");
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

std::pair<double, double> execute_maker_sell_chase(double qty, double price) {
    std::string err;
    std::string order_id = send_limit_order("SELL", qty, price, err);
    
    if (order_id.empty() && err.find("insufficient") != std::string::npos) {
        size_t pos = err.find("available: ");
        if (pos != std::string::npos) {
            size_t end_pos = err.find(")", pos + 11);
            if (end_pos != std::string::npos) {
                std::string true_qty = err.substr(pos + 11, end_pos - (pos + 11));
                qty = std::stod(true_qty);
                order_id = send_limit_order_raw("SELL", true_qty, price, err);
            }
        }
    }
    
    if (order_id.empty()) return {0.0, 0.0};
    
    bool fully_filled = false;
    for (int i = 0; i < CHASE_WAIT_SECONDS; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::string res = http_request(BASE_URL + "/v2/orders/" + order_id, "GET");
        try {
            auto j = json::parse(res);
            if (j["status"].get<std::string>() == "filled") { fully_filled = true; break; }
        } catch (...) {}
    }
    
    if (!fully_filled) {
        http_request(BASE_URL + "/v2/orders/" + order_id, "DELETE");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    double filled_q = 0.0; double filled_p = 0.0;
    std::string final_res = http_request(BASE_URL + "/v2/orders/" + order_id, "GET");
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

void alrm_handler(int) { intime = false; }

std::string readline_pipe(int fd) {
    std::string line; char c;
    while (read(fd, &c, 1) > 0) { if (c == '\n') break; line += c; }
    return line;
}

int main() {
    signal(SIGALRM, alrm_handler);

    if (const char* env_p = std::getenv("APCA_API_KEY_ID")) API_KEY = env_p;
    if (const char* env_p = std::getenv("APCA_API_SECRET_KEY")) API_SECRET = env_p;

    int p_to[2], p_from[2]; pipe(p_to); pipe(p_from);
    if (fork() == 0) { 
        dup2(p_to[0], STDIN_FILENO); dup2(p_from[1], STDOUT_FILENO); 
        execlp(BRAIN_EXEC.c_str(), "brain", nullptr); exit(1); 
    }
    close(p_to[0]); close(p_from[1]);

    ini:
    std::cout << "[INIT] Unificant i inicialitzant..." << std::endl;
    http_request(BASE_URL + "/v2/orders", "DELETE"); 
    for (int i = 0; i < 15; i++) {
        std::string check_res = http_request(BASE_URL + "/v2/orders?status=open", "GET");
        if (check_res == "[]" || check_res.empty()) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    double f_price = get_price();
    double f_cash = get_cash();
    Position init_p = get_position();
    
    if (f_price > 0 && f_cash > -1) {
        f_price_init = f_price;
        std::string init_msg = "INFO " + std::to_string(f_cash) + ";" + std::to_string(f_price) + ";" + std::to_string(init_p.qty) + "\n";
        write(p_to[1], init_msg.c_str(), init_msg.size());
        
        // ELIMINAT: El bloc perillós que agafava tot el teu saldo i el posava a la venda
        
    } else {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        goto ini;
    }

    while (true) {
        double price = get_price();
        double cash = get_cash();
        Position pos = get_position();

        if (price > 0 && cash > -1) {
            update_status_file(cash, pos.qty, price);

            std::cout << "[" << get_timestamp() << "] 🟢 ACTIU -> Preu: " << price << " $ | Caixa: " << cash << " $ | SOL: " << pos.qty << std::endl;

            review_sell_orders(p_to[1], cash, pos.qty);
            review_buy_orders(p_to[1], cash, pos.qty);

            std::string info = "INFO " + std::to_string(cash) + ";" + std::to_string(price) + ";" + std::to_string(pos.qty) + "\n";
            write(p_to[1], info.c_str(), info.size());

            intime = true; alarm(2);
            std::string response = readline_pipe(p_from[0]);
            alarm(0);

            if (!response.empty() && intime) {
                std::stringstream ss(response);
                std::string type; 
                
                if (ss >> type) {
                    if (type == "CMD") {
                        std::string cmd;
                        if (ss >> cmd) {
                            if (cmd == "FREEZE") {
                                // 1. Cancel·la totes les ofertes pendents
                                http_request(BASE_URL + "/v2/orders", "DELETE");
                                sell_orders_queue.clear(); buy_orders_queue.clear();
                                
                                // 2. Venda d'emergència (Pànic) de tota la cripto disponible
                                Position p = get_position();
                                if (p.qty_available > 0.001) {
                                    std::string e;
                                    // Ven a un 2% per sota del preu per forçar una execució Taker immediata
                                    send_limit_order_raw("SELL", p.raw_qty_available, price * 0.98, e); 
                                }
                            } else if (cmd == "UNFREEZE") {
                                // 1. Neteja de seguretat
                                http_request(BASE_URL + "/v2/orders", "DELETE");
                                sell_orders_queue.clear(); buy_orders_queue.clear();
                                std::this_thread::sleep_for(std::chrono::seconds(3));
                                
                                // 2. Càlcul del patrimoni total per fer el 50/50
                                double current_cash = get_cash();
                                Position p = get_position();
                                double current_equity = current_cash + (p.qty * price);
                                
                                // 3. Calculem quants SOL necessitem per tenir exactament la meitat del patrimoni
                                double target_qty = (current_equity * 0.5) / price;
                                std::string e;
                                
                                // 4. Executem la compra (o venda) per quadrar la proporció
                                if (target_qty > p.qty + 0.001) {
                                    double buy_qty = target_qty - p.qty;
                                    send_limit_order_raw("BUY", format_qty(buy_qty), price * 1.02, e); // Compra agressiva
                                } else if (target_qty < p.qty - 0.001) {
                                    double sell_qty = p.qty - target_qty;
                                    send_limit_order_raw("SELL", format_qty(sell_qty), price * 0.98, e); // Venda agressiva
                                }
                            }
                        }
                    } else if (type == "VALL") {
                        double q, entry_r, exit_r;
                        if (ss >> q >> entry_r >> exit_r) {
                            double entry_p = (entry_r < 2.0) ? f_price_init * entry_r : entry_r;
                            double exit_p = (exit_r < 2.0) ? f_price_init * exit_r : exit_r;
                            
                            http_request(BASE_URL + "/v2/orders", "DELETE");
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));

                            auto res = execute_maker_buy_chase(q, entry_p);
                            
                            if (res.first > 0) {
                                log_to_csv("VALL_IN (Buy)", res.second, res.first, cash - (res.first * res.second), pos.qty + res.first);
                                write(p_to[1], "BOUGHT 0\n", 9);
                                
                                double qty_to_sell = res.first; // NOMÉS VEN EL QUE ACABA DE COMPRAR
                                std::string err_msg;
                                std::string s_id = send_limit_order("SELL", qty_to_sell, exit_p, err_msg);
                                
                                if (s_id.empty() && err_msg.find("insufficient") != std::string::npos) {
                                    size_t pos = err_msg.find("available: ");
                                    if (pos != std::string::npos) {
                                        size_t end_pos = err_msg.find(")", pos + 11);
                                        if (end_pos != std::string::npos) {
                                            std::string true_qty = err_msg.substr(pos + 11, end_pos - (pos + 11));
                                            std::string d_err;
                                            s_id = send_limit_order_raw("SELL", true_qty, exit_p, d_err);
                                            if (!s_id.empty()) qty_to_sell = std::stod(true_qty);
                                        }
                                    }
                                }

                                if (s_id.empty()) sell_orders_queue.push_back({"", qty_to_sell, exit_p, 0.0});
                                else sell_orders_queue.push_back({s_id, qty_to_sell, exit_p, 0.0});
                            }
                        }
                    } else if (type == "PIC") {
                        double q, entry_r, exit_r;
                        if (ss >> q >> entry_r >> exit_r) {
                            double entry_p = (entry_r < 2.0) ? f_price_init * entry_r : entry_r;
                            double exit_p = (exit_r < 2.0) ? f_price_init * exit_r : exit_r;

                            http_request(BASE_URL + "/v2/orders", "DELETE");
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));

                            auto res = execute_maker_sell_chase(q, entry_p);
                            
                            if (res.first > 0) {
                                log_to_csv("PIC_IN (Sell)", res.second, res.first, cash + (res.first * res.second), pos.qty - res.first);
                                write(p_to[1], "SOLD 0\n", 7);
                                
                                double qty_to_buy = (res.first * res.second) / exit_p; // NOMÉS RECOMPRA EL QUE ACABA DE VENDRE
                                std::string err_msg;
                                std::string b_id = send_limit_order("BUY", qty_to_buy, exit_p, err_msg);
                                
                                if (b_id.empty() && err_msg.find("insufficient") != std::string::npos) {
                                    double safe_cash = get_cash();
                                    if (safe_cash > 1.0) {
                                        qty_to_buy = (safe_cash * 0.99) / exit_p;
                                        std::string d_err;
                                        b_id = send_limit_order_raw("BUY", format_qty(qty_to_buy), exit_p, d_err);
                                    }
                                }

                                if (b_id.empty()) buy_orders_queue.push_back({"", qty_to_buy, exit_p, 0.0});
                                else buy_orders_queue.push_back({b_id, qty_to_buy, exit_p, 0.0});
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

