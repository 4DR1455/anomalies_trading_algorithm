#pragma once
#include <string>

std::string format_qty(double val);
std::string format_price(double val);
std::string get_timestamp();
void log_to_csv(const std::string& type, double price, double qty, double cash, double shares_held);
void update_status_file(double cash, double shares, double price);
