#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/syscall.h>

using json = nlohmann::json;

// --- CONFIGURATION ---
const std::string BRAIN_EXEC = "./brain";
const std::string SYMBOL = "DOGE/USD";       
const std::string ASSET_SYMBOL = "DOGEUSD";  
const int SLEEP_SECONDS = 300; 
const int MAX_WAIT_SECONDS = 60;

// Use QUOTES to avoid price hallucinations
const std::string BASE_URL = "https://paper-api.alpaca.markets";
const std::string DATA_URL = "https://data.alpaca.markets/v1beta3/crypto/us/latest/quotes";

std::string API_KEY;
std::string API_SECRET;

bool intime;

// --- UTILITIES ---
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
        } else if (method == "DELETE") {
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

double parse_quantity(const std::string& order) {
    try {
        size_t space_pos = order.find(' ');
        if (space_pos != std::string::npos) return std::stod(order.substr(space_pos + 1));
        return 0.0; 
    } catch (...) { return 0.0; }
}

// --- API INFO ---
double get_price() {
    std::string response = http_request(DATA_URL + "?symbols=" + SYMBOL, "GET");
    try {
        auto json_data = json::parse(response);
        // Using Quotes (Bid/Ask) to prevent price errors
        if (json_data.contains("quotes") && json_data["quotes"].contains(SYMBOL)) {
            double bid = json_data["quotes"][SYMBOL]["bp"].get<double>();
            double ask = json_data["quotes"][SYMBOL]["ap"].get<double>();
            return (bid + ask) / 2.0;
        }
    } catch (...) {}
    return 0.0;
}

double get_cash() {
    std::string response = http_request(BASE_URL + "/v2/account", "GET");
    try {
        auto json_data = json::parse(response);
        if (json_data.contains("cash")) return std::stod(json_data["cash"].get<std::string>());
    } catch (...) {}
    return 0.0;
}

double get_shares() {
    std::string response = http_request(BASE_URL + "/v2/positions/" + ASSET_SYMBOL, "GET");
    if (response.find("code") != std::string::npos) return 0.0;
    try {
        auto json_data = json::parse(response);
        if (json_data.contains("qty")) return std::stod(json_data["qty"].get<std::string>());
    } catch (...) {} 
    return 0.0;
}

// --- ORDER MANAGEMENT (SMART LOOP) ---

void cancel_order(const std::string& order_id) {
    std::cout << "[Alpaca] CANCELLING remaining order " << order_id << "..." << std::endl;
    http_request(BASE_URL + "/v2/orders/" + order_id, "DELETE");
}

std::string get_order_status(const std::string& order_id) {
    std::string response = http_request(BASE_URL + "/v2/orders/" + order_id, "GET");
    try {
        auto json = json::parse(response);
        if (json.contains("status")) return json["status"].get<std::string>();
    } catch (...) {}
    return "unknown";
}

// Function to check real filled quantity
double get_filled_qty(const std::string& order_id) {
    std::string response = http_request(BASE_URL + "/v2/orders/" + order_id, "GET");
    try {
        auto json = json::parse(response);
        if (json.contains("filled_qty")) {
            return std::stod(json["filled_qty"].get<std::string>());
        }
    } catch (...) {}
    return 0.0;
}

// Returns the ACTUALLY executed quantity (0.0 if failed)
double place_and_confirm_order(const std::string& side, double qty, double limit_price) {
    json order_json;
    order_json["symbol"] = SYMBOL;
    order_json["qty"] = std::to_string(qty);
    order_json["side"] = (side == "BUY" ? "buy" : "sell");
    order_json["type"] = "limit"; 
    order_json["limit_price"] = std::to_string(limit_price);
    order_json["time_in_force"] = "gtc";

    std::string body = order_json.dump();
    std::cout << "[Alpaca] SENDING: " << side << " " << qty << " @ " << limit_price << std::endl;
    
    std::string response = http_request(BASE_URL + "/v2/orders", "POST", body);
    
    std::string order_id = "";
    try {
        auto resp_json = json::parse(response);
        if (resp_json.contains("id")) {
            order_id = resp_json["id"].get<std::string>();
        } else {
            std::cout << "[Alpaca ERROR] " << response << std::endl;
            return 0.0; // Total error
        }
    } catch (...) { return 0.0; }

    std::cout << "[Alpaca] Waiting for execution (max " << MAX_WAIT_SECONDS << "s)..." << std::endl;
    
    // --- SMART LOOP ---
    bool fully_filled = false;
    for (int i = 0; i < MAX_WAIT_SECONDS; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        std::string status = get_order_status(order_id);
        if (status == "filled") {
            fully_filled = true;
            std::cout << "[Alpaca] STATUS: FILLED (in " << i+1 << "s)" << std::endl;
            break;
        }
        if (status == "canceled" || status == "rejected" || status == "expired") {
            std::cout << "[Alpaca] STATUS: " << status << ". Stopping wait." << std::endl;
            break; 
        }
    }

    if (fully_filled) {
        return qty; // Success
    } else {
        // Timeout or external cancellation.
        // 1. Check fill so far
        double partial_fill = get_filled_qty(order_id);
        
        // 2. Kill order if still open
        std::string status = get_order_status(order_id);
        if (status != "filled" && status != "canceled" && status != "rejected") {
            cancel_order(order_id);
            // Wait for cancellation to process
            std::this_thread::sleep_for(std::chrono::seconds(2));
            partial_fill = get_filled_qty(order_id); // Update last second fills
        }

        if (partial_fill > 0) {
            std::cout << "[Alpaca] PARTIALLY FILLED: " << partial_fill << " / " << qty << std::endl;
            return partial_fill;
        } else {
            std::cout << "[Alpaca] Nothing executed. Canceled." << std::endl;
            return 0.0;
        }
    }
}

void sync_initial_position(int pipe_fd) {
    std::cout << "[INIT] Checking open positions..." << std::endl;
    std::string response = http_request(BASE_URL + "/v2/positions/" + ASSET_SYMBOL, "GET");
    try {
        auto json = json::parse(response);
        if (!json.contains("code") && json.contains("qty")) {
            double qty = std::stod(json["qty"].get<std::string>());
            double avg_price = std::stod(json["avg_entry_price"].get<std::string>());
            if (qty > 0) {
                std::cout << "[INIT] Position found: " << qty << " shares at " << avg_price << "$" << std::endl;
                std::string confirm = "BOUGHT " + std::to_string(qty) + " " + std::to_string(avg_price) + "\n";
                write(pipe_fd, confirm.c_str(), confirm.size());
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    } catch (...) {}
}

// --- MAIN ---
std::string readline_pipe(int fd) {
    std::string line;
    char c;
    while (read(fd, &c, 1) > 0) {
        if (c == '\n') break;
        line += c;
    }
    return line;
}

double get_last_equity() {
    std::string response = http_request(BASE_URL + "/v2/account", "GET");
    try {
        auto json_data = json::parse(response);
        if (json_data.contains("last_equity")) 
            return std::stod(json_data["last_equity"].get<std::string>());
    } catch (...) {}
    return 0.0;
}

// --- DASHBOARD UPDATE FUNCTION ---
void update_dashboard_file(double equity, double cash, double shares, double price, double last_equity) {
    std::ofstream file("status.json", std::ios::trunc);
    if (file.is_open()) {
        file << "{\n";
        file << "  \"equity\": " << std::fixed << std::setprecision(2) << equity << ",\n";
        file << "  \"cash\": " << cash << ",\n";
        file << "  \"invested\": " << (shares * price) << ",\n";
        file << "  \"price\": " << std::setprecision(4) << price << ",\n";
        file << "  \"last_equity\": " << std::setprecision(2) << last_equity << "\n";
        file << "}\n";
        file.close();
    }
}
void alrm_handler(int signum) { intime = false; }

int main() {
    std::ifstream check_file("data.csv");
    if (!check_file.good()) {
        std::ofstream outfile("data.csv");
        outfile << "timestamp,action,price,qty,cash_available,shares_held\n";
    }

    struct sigaction sa; sa.sa_handler = alrm_handler; sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, nullptr);

    if (const char* env_p = std::getenv("APCA_API_KEY_ID")) API_KEY = env_p;
    if (const char* env_p = std::getenv("APCA_API_SECRET_KEY")) API_SECRET = env_p;
    if (API_KEY.empty()) return 1;

    int pipe_to_brain[2], pipe_from_brain[2];
    if (pipe(pipe_to_brain) < 0 || pipe(pipe_from_brain) < 0) return 1;

    pid_t pid = fork();
    if (pid == 0) { // CHILD
        dup2(pipe_to_brain[0], STDIN_FILENO);
        dup2(pipe_from_brain[1], STDOUT_FILENO);
        close(pipe_to_brain[1]); close(pipe_from_brain[0]); close(pipe_to_brain[0]); close(pipe_from_brain[1]);
        execlp(BRAIN_EXEC.c_str(), "brain", nullptr);
        exit(1);
    }
    close(pipe_to_brain[0]); close(pipe_from_brain[1]);

    std::cout << "Bot Started. SMART PARTIAL FILLS MODE." << std::endl;

    sync_initial_position(pipe_to_brain[1]);

    while (true) {
        double price = get_price();
        double cash = get_cash();
        double shares = get_shares();
        
        bool round_success = true; 

        if (price > 0) {
            std::cout << "[STATUS] " << get_timestamp() << " | P: " << price << " | $: " << cash << std::endl;
            
            // INFO PIPE
            std::string msg = "INFO " + std::to_string(cash) + ";" + std::to_string(price) + ";" + std::to_string(shares) + "\n"; 
            write(pipe_to_brain[1], msg.c_str(), msg.size());

            // Wait for Brain Decision
            intime = true;
            alarm(2);
            std::string order = readline_pipe(pipe_from_brain[0]);
            alarm(0);
            if (!order.empty() && intime) {
                double qty_requested = parse_quantity(order);
                double qty_executed = 0.0;
                double last_equity = get_last_equity(); 

                // --- BUY BLOCK ---
                if (order.find("BUY") != std::string::npos && qty_requested > 0) {
                     qty_executed = place_and_confirm_order("BUY", qty_requested, price);
                     
                     if (qty_executed > 0) {
                         // MATH CALCULATION OF NEW STATE (Instant update)
                         double cost = qty_executed * price;
                         double new_cash = cash - cost;      
                         double new_shares = shares + qty_executed;
                         double new_equity = new_cash + (new_shares * price);

                         // 1. CSV: Save post-operation reality
                         log_to_csv("BUY", price, qty_executed, new_cash, new_shares);
                         
                         // 2. DASHBOARD: Update web instantly
                         update_dashboard_file(new_equity, new_cash, new_shares, price, last_equity);

                         // 3. Confirm to Brain
                         std::string confirm = "BOUGHT " + std::to_string(qty_executed) + " " + std::to_string(price) + "\n";
                         write(pipe_to_brain[1], confirm.c_str(), confirm.size());
                     } else {
                         std::cout << "[ROLLBACK] Nothing bought." << std::endl;
                         std::string rb = "ROLLBACK\n";
                         write(pipe_to_brain[1], rb.c_str(), rb.size());
                         round_success = false;
                     }
                } 
                // --- SELL BLOCK ---
                else if (order.find("SELL") != std::string::npos && qty_requested > 0) {
                     qty_executed = place_and_confirm_order("SELL", qty_requested, price);
                     
                     if (qty_executed > 0) {
                         // MATH CALCULATION OF NEW STATE
                         double profit = qty_executed * price;
                         double new_cash = cash + profit;    
                         double new_shares = shares - qty_executed;
                         double new_equity = new_cash + (new_shares * price);

                         // 1. CSV
                         log_to_csv("SELL", price, qty_executed, new_cash, new_shares);

                         // 2. DASHBOARD
                         update_dashboard_file(new_equity, new_cash, new_shares, price, last_equity);

                         // 3. Confirm to Brain
                         std::string confirm = "SOLD " + std::to_string(qty_executed) + " " + std::to_string(price) + "\n";
                         write(pipe_to_brain[1], confirm.c_str(), confirm.size());
                     } else {
                         std::cout << "[ROLLBACK] Nothing sold." << std::endl;
                         std::string rb = "ROLLBACK\n";
                         write(pipe_to_brain[1], rb.c_str(), rb.size());
                         round_success = false;
                     }
                }
            }            
        }
        
        if (round_success) {
            std::this_thread::sleep_for(std::chrono::seconds(SLEEP_SECONDS));
        } else {
            std::cout << "[SKIP] Retrying soon..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
    return 0;
}