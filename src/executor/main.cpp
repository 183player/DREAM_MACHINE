// src/executor/main.cpp
#include "logger.h"
#include "pipe.h"
#include "constants.h"

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>

using namespace dream_machine;

namespace {

bool parsePipeHandleFromArgs(int argc, char* argv[], uintptr_t& out_handle) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        const std::string prefix = "--pipe-handle=";
        if (arg.find(prefix) == 0) {
            std::string value_str = arg.substr(prefix.length());
            try {
                out_handle = std::stoull(value_str);
                return true;
            } catch (const std::exception&) {
                LOG_ERROR("Failed to parse --pipe-handle value: " + value_str);
                return false;
            }
        }
    }
    return false;
}

} // namespace

int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("executor");
    LOG_INFO("=== Dream Machine Executor starting ===");

    uintptr_t pipe_handle_value = 0;
    if (parsePipeHandleFromArgs(argc, argv, pipe_handle_value)) {
        LOG_INFO("Received --pipe-handle: " + std::to_string(pipe_handle_value));
    } else {
        LOG_INFO("No --pipe-handle provided, connecting via pipe name");
    }

    // 连接到 launcher 的 executor 专用管道
    std::string pipe_name_str = pipe_names::launcher_executor();
    std::wstring pipe_name(pipe_name_str.begin(), pipe_name_str.end());

    LOG_INFO("Connecting to launcher pipe: " + pipe_name_str);

    NamedPipe pipe;
    if (!pipe.connect(pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to launcher pipe");
        return 1;
    }

    LOG_INFO("Connected to launcher pipe");

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
            LOG_WARN("Executor detected pipe is not connected, exiting main loop");
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
                LOG_WARN("readLine returned BROKEN, exiting main loop");
                break;
            } else if (read_result == PipeResult::TIMEOUT) {
                continue;
            } else {
                LOG_WARN("readLine returned unexpected error: " + std::to_string(static_cast<int>(read_result)));
                break;
            }
        } else if (peek_result == PipeResult::BROKEN) {
            LOG_WARN("peekAvailable returned BROKEN, exiting main loop");
            break;
        } else if (peek_result == PipeResult::WOULD_BLOCK) {
            // 无数据，正常
        } else if (peek_result != PipeResult::OK) {
            LOG_WARN("peekAvailable returned unexpected error: " + std::to_string(static_cast<int>(peek_result)));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    LOG_INFO("Executor main loop exited, shutting down...");
    pipe.close();
    LOG_INFO("=== Executor exited ===");
    return 0;
}