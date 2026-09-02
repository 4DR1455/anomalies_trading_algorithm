#pragma once
#include <string>
#include "Types.hpp"

class AlpacaAPI {
public:
    AlpacaAPI(const std::string& key, const std::string& secret);
    
    std::string http_request(const std::string& url, const std::string& method, const std::string& body = "");
    double get_price();
    double get_cash();
    Position get_position();
    
    std::string send_limit_order_raw(const std::string& side, const std::string& qty_str, double price, std::string& out_error);
    std::string send_limit_order(const std::string& side, double qty, double price, std::string& out_error);
    
    void delete_all_orders();
    void delete_order(const std::string& order_id);
    std::string get_open_orders();
    std::string get_order_status(const std::string& order_id);

private:
    std::string api_key;
    std::string api_secret;
};
