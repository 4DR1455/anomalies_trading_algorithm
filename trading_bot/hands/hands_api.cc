/**
 * ======================================================================================
 * HANDS API BRIDGE (C++17)
 * ======================================================================================
 * This component acts as the Execution Engine and Network Layer of the trading bot.
 *
 * Responsibilities:
 * 1. Market Connectivity: Handles all HTTP/REST communication with the Alpaca API.
 * 2. Order Management: Executes Buy/Sell orders with a "Precision Repair" mechanism.
 * 3. IPC Bridge: Spawns the OCaml 'Brain' process and communicates via standard Pipes.
 * 4. State Persistence: Logs trades to CSV and real-time status to JSON for the Dashboard.
 * ======================================================================================
 */

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

// --- CONFIGURATION CONSTANTS ---
const std::string BRAIN_EXEC = "./brain";          // Path to the OCaml strategy binary
const std::string SYMBOL = "SOL/USD";              // Trading pair (Crypto)
const std::string ASSET_SYMBOL = "SOLUSD";         // Asset identifier for Positions
const int SLEEP_SECONDS = 30;                      // Main cycle interval (Polling rate)
const int CHASE_WAIT_SECONDS = 20;                 // Timeout for filling buy orders

// API Endpoints (Alpaca Markets)
const std::string BASE_URL = "https://paper-api.alpaca.markets";
const std::string DATA_URL = "https://data.alpaca.markets/v1beta3/crypto/us/latest/quotes";

// Global Credentials & Runtime Parameters
std::string API_KEY;
std::string API_SECRET;
double PROFIT_MARGIN = 1.0;                        // Configured as percentage (e.g., 1.0 for 1%)

// Tracks the total quantity of shares currently locked in active sell orders
double shares_selling;

// --- DATA STRUCTURES ---

// Represents an active Sell Limit Order placed in the market
struct ActiveOrder {
    std::string id;       // Alpaca Order UUID
    std::string side;     // "SELL"
    double qty;           // Ordered Quantity
    double price;         // Limit Price
    double grid_id;       // Mapping to the Brain's internal grid system
};

// Represents the current portfolio holding snapshot
struct Position {
    double qty = 0.0;           // Total quantity in account
    double qty_available = 0.0; // Spendable quantity (not locked in orders)
    double avg_entry_price = 0.0;
};

// Queue of active sell orders.
// Uses std::list for safe O(1) removal of elements while iterating (iterator stability).
std::list<ActiveOrder> sell_orders_queue;

// Flag for pipe communication timeout handling
bool intime;

// --- UTILITIES & FORMATTING ---

/**
 * Formats double to string with 4 decimals (Alpaca Crypto requirement)
 * Note: Uses standard rounding. If "insufficient balance" occurs, Repair Logic triggers.
 */
std::string format_qty(double val) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(4) << val;
    return ss.str();
}

/**
 * Formats double to string with 2 decimals for USD prices
 */
std::string format_price(double val) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2) << val;
    return ss.str();
}

// Libcurl write callback to capture response data
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Generic HTTP Request Wrapper (Authenticated with Alpaca Headers)
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

// Returns formatted timestamp string
std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm* local_time = std::localtime(&now_time);
    std::stringstream ss;
    ss << std::put_time(local_time, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

// Appends trade details to the CSV log file
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

// Updates status.json for the strategy Dashboard
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

// --- ALPACA API INTERFACE ---

// Fetches the current market midpoint price
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

// Fetches the current cash balance
double get_cash() {
    std::string response = http_request(BASE_URL + "/v2/account", "GET");
    try { 
        auto j = json::parse(response);
        return std::stod(j["cash"].get<std::string>()); 
    } catch (...) { 
        return -1;
    }
}

// Fetches asset position with availability details (crucial for multi-order handling)
Position get_position() {
    Position pos;
    std::string response = http_request(BASE_URL + "/v2/positions/" + ASSET_SYMBOL, "GET");
    try { 
        auto j = json::parse(response);
        if (j.contains("qty")) {
            pos.qty = std::stod(j["qty"].get<std::string>());
            pos.avg_entry_price = std::stod(j["avg_entry_price"].get<std::string>());
            if (j.contains("qty_available")) {
                pos.qty_available = std::stod(j["qty_available"].get<std::string>());
            } else {
                pos.qty_available = pos.qty;
            }
        }
    } catch (...) {}
    return pos;
}

// Forward declaration for recursive repair
std::string send_limit_order_raw(const std::string& side, const std::string& qty_str, double price, bool retry = true);

/**
 * CORE ORDER SUBMISSION (with Precision Repair)
 * Extracts the exact available quantity from error messages to prevent "insufficient balance"
 * failures caused by floating point rounding.
 */
std::string send_limit_order_raw(const std::string& side, const std::string& qty_str, double price, bool retry) {
    json o; 
    o["symbol"] = SYMBOL; 
    o["qty"] = qty_str; 
    o["side"] = (side == "BUY" ? "buy" : "sell");
    o["type"] = "limit"; 
    o["limit_price"] = format_price(price);
    o["time_in_force"] = "gtc";
    
    std::string res = http_request(BASE_URL + "/v2/orders", "POST", o.dump());
    try { 
        auto j = json::parse(res); 
        if (j.contains("id")) return j["id"].get<std::string>(); 
        
        // Handle Alpaca 403 error for insufficient balance
        if (j.contains("code") && j["code"] == 40310000 && retry) {
            if (j.contains("available")) {
                std::string api_real_available = j["available"].get<std::string>();
                std::cout << "[REPAIR] Insufficient balance. Resubmitting with API exact string: " << api_real_available << std::endl;
                return send_limit_order_raw(side, api_real_available, price, false); 
            }
        }
        std::cerr << "[ERROR ALPACA] " << res << std::endl;
    } catch (...) {
        std::cerr << "[ERROR CRITICAL] Non-JSON API Response: " << res << std::endl;
    }
    return "";
}

// Entry point for standard numeric orders
std::string send_limit_order(const std::string& side, double qty, double price, bool retry = true) {
    return send_limit_order_raw(side, format_qty(qty), price, retry);
}

std::string get_order_status(const std::string& id) {
    std::string res = http_request(BASE_URL + "/v2/orders/" + id, "GET");
    try { return json::parse(res)["status"].get<std::string>(); } catch (...) { return "unknown"; }
}

double get_filled_avg_price(const std::string& id) {
    std::string res = http_request(BASE_URL + "/v2/orders/" + id, "GET");
    try { return std::stod(json::parse(res)["filled_avg_price"].get<std::string>()); } catch (...) { return 0.0; }
}

// --- LOGIC: ORDER MANAGEMENT ---

/**
 * Periodically reviews active sell orders.
 * If filled: Notifies the Brain to release the grid inventory.
 * If canceled/expired: Resubmits the order to maintain the strategy.
 */
void review_sell_orders(int pipe_to_brain, double current_cash, double current_shares) {
    auto it = sell_orders_queue.begin();
    while (it != sell_orders_queue.end()) {
        std::string status = get_order_status(it->id);
        if (status == "filled") {
            double filled_p = get_filled_avg_price(it->id);
            std::cout << "[SELL] Successful grid exit: " << it->grid_id << " @ " << filled_p << std::endl;
            log_to_csv("SELL", filled_p, it->qty, current_cash + (it->qty * filled_p), current_shares - it->qty);
            
            // Notify Brain: Release grid position
            std::string msg = "SOLD " + std::to_string(it->grid_id) + "\n";
            write(pipe_to_brain, msg.c_str(), msg.size());
            
            Position pos = get_position();
            shares_selling = pos.qty - pos.qty_available; 
            it = sell_orders_queue.erase(it);
        } else if (status == "canceled" || status == "expired") { 
            std::cout << "[SELL] Order " << it->id << " expired/canceled. Re-submitting..." << std::endl;
            std::string s_id = send_limit_order("SELL", it->qty, it->price);
            if (!s_id.empty()) {
                // List allows pushing back while iterating without invalidating 'it'
                sell_orders_queue.push_back({s_id, "SELL", it->qty, it->price, it->grid_id});
            }
            it = sell_orders_queue.erase(it); 
        } else { ++it; }
    }
}

/**
 * Executes a 'Chase' buy order (Limit at current price + timeout).
 */
std::pair<double, double> execute_buy_chase(double qty, double price) {
    std::string order_id = send_limit_order("BUY", qty, price);
    if (order_id.empty()) return {0.0, 0.0};
    
    std::cout << "[CHASE] Seeking fill for " << qty << " SOL..." << std::endl;
    for (int i = 0; i < CHASE_WAIT_SECONDS; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (get_order_status(order_id) == "filled") return {qty, get_filled_avg_price(order_id)};
    }
    
    std::cout << "[CHASE] Timeout reached. Canceling order." << std::endl;
    http_request(BASE_URL + "/v2/orders/" + order_id, "DELETE");
    return {0.0, 0.0};
}

// --- IPC HELPERS ---

void sync_params() {
    std::ifstream file("params.txt");
    if (file.is_open()) {
        std::string line;
        if (std::getline(file, line)) {
            std::stringstream ss(line);
            ss >> PROFIT_MARGIN;
        }
    }
}

void alrm_handler(int) { intime = false; }

std::string readline_pipe(int fd) {
    std::string line; char c;
    while (read(fd, &c, 1) > 0) { if (c == '\n') break; line += c; }
    return line;
}

// --- MAIN EXECUTION ENTRY POINT ---
int main() {
    std::cout << "[SYSTEM] Initializing Hands API Bridge..." << std::endl;
    struct sigaction sa; sa.sa_handler = alrm_handler; sa.sa_flags = 0; sigaction(SIGALRM, &sa, nullptr);
    
    if (const char* env_p = std::getenv("APCA_API_KEY_ID")) API_KEY = env_p;
    if (const char* env_p = std::getenv("APCA_API_SECRET_KEY")) API_SECRET = env_p;

    // IPC Setup: Forks the Brain process and connects pipes
    int p_to[2], p_from[2]; pipe(p_to); pipe(p_from);
    if (fork() == 0) { 
        dup2(p_to[0], STDIN_FILENO); dup2(p_from[1], STDOUT_FILENO);
        execlp(BRAIN_EXEC.c_str(), "brain", nullptr); exit(1);
    }
    close(p_to[0]); close(p_from[1]);

    shares_selling = 0;

    // 1. INITIAL SYNC: Load account state before starting the loop
    ini:
    double f_price = get_price();
    double f_cash = get_cash();
    Position init_p = get_position();
    
    if (f_price > 0) {
        std::cout << "[INIT] Base Price locked at: " << f_price << std::endl;
        std::string init_msg = "INFO " + std::to_string(f_cash) + ";" + std::to_string(f_price) + ";" + std::to_string(init_p.qty) + "\n";
        write(p_to[1], init_msg.c_str(), init_msg.size());
        
        if (init_p.qty > 0.01) {
            double g_id = init_p.avg_entry_price / f_price;
            std::cout << "[INIT] Found Position: " << init_p.qty << " SOL. GridID: " << g_id << std::endl;
            std::string bought_msg = "BOUGHT " + std::to_string(g_id) + "\n";
            write(p_to[1], bought_msg.c_str(), bought_msg.size());
            
            // Only place sell orders for shares not already on the market
            double to_sell = init_p.qty_available;
            if (to_sell > 0.01) {
                sync_params();
                // FIX: Correctly convert margin percentage to ratio
                double sell_target = init_p.avg_entry_price * (1.0 + (PROFIT_MARGIN / 100.0));
                std::string s_id = send_limit_order("SELL", to_sell, sell_target);
                if (!s_id.empty()) {
                    std::cout << "[INIT] Sell order placed at " << sell_target << " for " << to_sell << " SOL" << std::endl;
                    sell_orders_queue.push_back({s_id, "SELL", to_sell, sell_target, g_id});
                    shares_selling += to_sell;
                }
            } else {
                std::cout << "[INIT] Active position already managed by open Alpaca orders." << std::endl;
                shares_selling = init_p.qty; 
            }
        }
    }
    else {
        std::this_thread::sleep_for(std::chrono::seconds(SLEEP_SECONDS));
        goto ini;
    }

    // 2. MAIN POLLING LOOP
    while (true) {
        bool done = true;
        auto start_cycle = std::chrono::steady_clock::now();
        sync_params();
        double price = get_price();
        double cash = get_cash();
        Position current_pos = get_position();
        double total_shares = current_pos.qty;

        if (price > 0 && cash > -1) {
            update_status_file(cash, total_shares, price);
            review_sell_orders(p_to[1], cash, total_shares);

            std::cout << "[" << get_timestamp() << "] P: " << price << " | $: " << cash << " | S: " << total_shares << std::endl;

            // IPC: Send market snapshot to Brain
            std::string info = "INFO " + std::to_string(cash) + ";" + std::to_string(price) + ";" + std::to_string(total_shares) + "\n"; 
            write(p_to[1], info.c_str(), info.size());

            // IPC: Wait for Decision (with timeout)
            intime = true; alarm(2);
            std::string response = readline_pipe(p_from[0]);
            alarm(0);

            if (!response.empty() && intime) {
                std::stringstream ss(response);
                std::string type; double q; double ratio_sell;
                if (ss >> type >> q >> ratio_sell) {
                    if (type == "BUY" && q > 0) {
                        std::cout << "[BRAIN] BUY SIGNAL: " << q << " SOL" << std::endl;
                        auto result = execute_buy_chase(q, price);
                        
                        if (result.first > 0) {
                            // Sincronització: Use (average buy / initial price) as grid_id
                            double g_id = (result.second/f_price); 
                            log_to_csv("BUY", result.second, result.first, cash - (result.first * result.second), total_shares + result.first);
                            
                            std::string confirm = "BOUGHT " + std::to_string(g_id) + "\n";
                            write(p_to[1], confirm.c_str(), confirm.size());
                            
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                            
                            // Place sell order using REAL available quantity post-fees
                            Position pos_after = get_position();
                            double available_to_sell = pos_after.qty_available;
                            
                            if (available_to_sell > 0) {
                                double sell_target = f_price * ratio_sell;
                                std::string s_id = send_limit_order("SELL", available_to_sell, sell_target);
                                if (!s_id.empty()) {
                                    std::cout << "[SELL] Bait set at " << sell_target << " for " << available_to_sell << " SOL" << std::endl;
                                    sell_orders_queue.push_back({s_id, "SELL", available_to_sell, sell_target, g_id});
                                    shares_selling += available_to_sell;
                                }
                            }
                        }
                        else {
                            // Rollback Brain state if purchase failed
                            std::cout << "[SYSTEM] Chase failed. Notifying Brain." << std::endl;
                            write(p_to[1], "ROLLBACK\n", 9);
                            done = false; 
                        }
                    }
                }
            }
        }
        
        // Dynamic wait: deduct processing time from the 30s heart rate
        if (done && cash > -1) {
            auto end_cycle = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_cycle - start_cycle).count();
            long remaining_sleep = (SLEEP_SECONDS * 1000) - elapsed;
            if (remaining_sleep > 0) std::this_thread::sleep_for(std::chrono::milliseconds(remaining_sleep));
        }
    }
    return 0;
}