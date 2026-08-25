#include <iostream>
#include <thread>
#include <chrono>

int main() {
    std::cout << "Process [monitor] started" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::cout << "Process [monitor] exiting" << std::endl;
    return 0;
}