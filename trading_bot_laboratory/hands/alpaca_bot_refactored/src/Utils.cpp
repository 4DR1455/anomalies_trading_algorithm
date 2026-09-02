#include "Utils.hpp"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

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

std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm* local_time = std::localtime(&now_time);
    std::stringstream ss;
    ss << std::put_time(local_time, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void log_to_csv(const std::string& type, double price, double qty, double cash, double shares_held) {
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
