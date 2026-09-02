#pragma once
#include <string>

class BrainCommunicator {
public:
    BrainCommunicator(const std::string& exec_path);
    void start();
    void send_message(const std::string& msg);
    std::string read_message_with_timeout(int timeout_seconds, bool& success);

private:
    std::string exec_path;
    int p_to[2];
    int p_from[2];
};
