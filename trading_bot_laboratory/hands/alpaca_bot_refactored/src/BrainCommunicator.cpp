#include "BrainCommunicator.hpp"
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <iostream>

volatile bool g_intime = false;

void internal_alrm_handler(int) { 
    g_intime = false; 
}

BrainCommunicator::BrainCommunicator(const std::string& exec_path) : exec_path(exec_path) {
    signal(SIGALRM, internal_alrm_handler);
}

void BrainCommunicator::start() {
    pipe(p_to); pipe(p_from);
    if (fork() == 0) { 
        dup2(p_to[0], STDIN_FILENO); 
        dup2(p_from[1], STDOUT_FILENO); 
        execlp(exec_path.c_str(), "brain", nullptr); 
        exit(1); 
    }
    close(p_to[0]); close(p_from[1]);
}

void BrainCommunicator::send_message(const std::string& msg) {
    write(p_to[1], msg.c_str(), msg.size());
}

std::string BrainCommunicator::read_message_with_timeout(int timeout_seconds, bool& success) {
    g_intime = true; 
    alarm(timeout_seconds);

    std::string line; 
    char c;
    while (read(p_from[0], &c, 1) > 0) { 
        if (c == '\n') break; 
        line += c; 
    }

    alarm(0);
    success = g_intime;
    return line;
}
