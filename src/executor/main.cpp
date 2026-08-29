// src/executor/main.cpp
#include "logger.h"
#include "pipe.h"
#include "constants.h"

#include <iostream>
#include <string>
#include <thread>
#include <chrono>

using namespace dream_machine;

int main() {
    Logger::instance().setProcessName("executor");
    LOG_INFO("=== Dream Machine Executor starting ===");

    // 通过名称连接 launcher
    std::string pipe_name_str = pipe_names::launcher_executor();
    std::wstring pipe_name(pipe_name_str.begin(), pipe_name_str.end());

    LOG_INFO("Connecting to launcher pipe: " + pipe_name_str);

    NamedPipe pipe;
    if (!pipe.connect(pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to launcher pipe");
        return 1;
    }

    LOG_INFO("Connected to launcher pipe");

    // 发送注册消息
    std::string register_msg = R"({"type":"register","process":"executor"})";
    if (pipe.writeLine(register_msg) != PipeResult::OK) {
        LOG_ERROR("Failed to send registration message");
        return 1;
    }

    LOG_INFO("Registration message sent: " + register_msg);

    LOG_INFO("Entering main loop...");

    int loop_count = 0;
    while (true) {
        ++loop_count;
        if (loop_count % 100 == 0) {
            LOG_INFO("Executor main loop heartbeat: " + std::to_string(loop_count) + " iterations");
        }

        if (!pipe.isConnected()) {
            LOG_WARN("Launcher pipe disconnected, exiting");
            break;
        }

        DWORD bytes_available = 0;
        PipeResult peek_result = pipe.peekAvailable(bytes_available);

        if (peek_result == PipeResult::OK && bytes_available > 0) {
            std::string message;
            PipeResult read_result = pipe.readLine(message, 3000);

            if (read_result == PipeResult::OK) {
                LOG_INFO("Received: " + message);
                // TODO: 执行原子操作（EXE-01 ~ EXE-10）
            } else if (read_result == PipeResult::BROKEN) {
                LOG_WARN("Pipe broken, exiting");
                break;
            } else if (read_result == PipeResult::TIMEOUT) {
                continue;
            } else {
                LOG_WARN("Read error: " + std::to_string(static_cast<int>(read_result)));
                break;
            }
        } else if (peek_result == PipeResult::BROKEN) {
            LOG_WARN("Pipe broken, exiting");
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    LOG_INFO("Executor shutting down...");
    pipe.close();
    LOG_INFO("=== Executor exited ===");
    return 0;
}