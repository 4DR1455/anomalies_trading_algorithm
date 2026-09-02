/**
 * ============================================================================
 * AlpacaAPI Implementation (Laboratory Version)
 * ============================================================================
 * Handles REST communication with Alpaca using libcurl. This lightweight 
 * version acts purely as a transport layer, returning raw payloads and letting 
 * the OrderManager handle high-level exception recovery and validation.
 * ============================================================================
 */

#include "AlpacaAPI.hpp"
#include "Utils.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <iostream>

using json = nlohmann::json;

// Libcurl write callback for buffering response data
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

AlpacaAPI::AlpacaAPI(const std::string& key, const std::string& secret) 
    : api_key(key), api_secret(secret) {}

// Executes core HTTP GET, POST, and DELETE requests
std::string AlpacaAPI::http_request(const std::string& url, const std::string& method, const std::string& body) {
    CURL* curl = curl_easy_init();
    std::string readBuffer;
    if(curl) {
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, ("APCA-API-KEY-ID: " + api_key).c_str());
        headers = curl_slist_append(headers, ("APCA-API-SECRET-KEY: " + api_secret).c_str());
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

// Fetches the current mid-price for the target asset
double AlpacaAPI::get_price() {
    std::string response = http_request(DATA_URL + "?symbols=" + SYMBOL, "GET");
    try {
        auto j = json::parse(response);
        if (j.contains("quotes") && j["quotes"].contains(SYMBOL)) {
            return (j["quotes"][SYMBOL]["bp"].get<double>() + j["quotes"][SYMBOL]["ap"].get<double>()) / 2.0;
        }
    } catch (...) {}
    return 0.0;
}

// Fetches current account cash balance
double AlpacaAPI::get_cash() {
    std::string response = http_request(BASE_URL + "/v2/account", "GET");
    try { 
        auto j = json::parse(response);
        return std::stod(j["cash"].get<std::string>()); 
    } catch (...) { return -1; }
}

// Retrieves current asset holdings and availability
Position AlpacaAPI::get_position() {
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

// Submits a low-level limit order directly to the broker
std::string AlpacaAPI::send_limit_order_raw(const std::string& side, const std::string& qty_str, double price, std::string& out_error) {
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

// Standard wrapper for sending limit orders with pre-formatted quantities
std::string AlpacaAPI::send_limit_order(const std::string& side, double qty, double price, std::string& out_error) {
    return send_limit_order_raw(side, format_qty(qty), price, out_error);
}

void AlpacaAPI::delete_all_orders() {
    http_request(BASE_URL + "/v2/orders", "DELETE");
}

void AlpacaAPI::delete_order(const std::string& order_id) {
    http_request(BASE_URL + "/v2/orders/" + order_id, "DELETE");
}

std::string AlpacaAPI::get_open_orders() {
    return http_request(BASE_URL + "/v2/orders?status=open", "GET");
}

std::string AlpacaAPI::get_order_status(const std::string& order_id) {
    return http_request(BASE_URL + "/v2/orders/" + order_id, "GET");
}
