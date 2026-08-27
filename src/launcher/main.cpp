#include <iostream>
#include <thread>
#include <chrono>
#include "logger.h"

int main() {
    using namespace dream_machine;
    Logger::instance().setProcessName("launcher");
    LOG_INFO("Hello from launcher!");
    std::cout << "Launcher started" << std::endl;
    std::cout << "Process [launcher] started" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::cout << "Process [launcher] exiting" << std::endl;
    return 0;
}