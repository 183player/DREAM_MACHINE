#include <iostream>
#include <thread>
#include <chrono>

int main() {
    std::cout << "Process [launcher] started" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::cout << "Process [launcher] exiting" << std::endl;
    return 0;
}