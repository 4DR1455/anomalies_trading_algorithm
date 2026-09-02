#pragma once
#include <string>

const std::string BRAIN_EXEC = "./brain";
const std::string SYMBOL = "SOL/USD";
const std::string ASSET_SYMBOL = "SOLUSD";
const int SLEEP_SECONDS = 30;
const int CHASE_WAIT_SECONDS = 20;

const std::string BASE_URL = "https://paper-api.alpaca.markets";
const std::string DATA_URL = "https://data.alpaca.markets/v1beta3/crypto/us/latest/quotes";

struct ActiveOrder {
    std::string id;
    std::string side;
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