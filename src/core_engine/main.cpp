#include <iostream>
#include <thread>
#include <chrono>

int main() {
    std::cout << "Process [core_engine] started" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::cout << "Process [core_engine] exiting" << std::endl;
    return 0;
}