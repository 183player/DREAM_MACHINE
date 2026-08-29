// src/monitor/main.cpp
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
    Logger::instance().setProcessName("monitor");
    LOG_INFO("=== Dream Machine Monitor starting ===");

    // 1. 解析命令行参数，获取父进程传递的管道句柄
    uintptr_t handle_value = 0;
    if (!parsePipeHandleFromArgs(argc, argv, handle_value)) {
        LOG_ERROR("No --pipe-handle provided, cannot connect to launcher");
        return 1;
    }

    LOG_INFO("Received --pipe-handle: " + std::to_string(handle_value));

    // 2. 接管父进程传递的管道句柄
    NamedPipe pipe = NamedPipe::adopt(handle_value);
    if (!pipe.isValid()) {
        LOG_ERROR("Failed to adopt pipe handle: " + std::to_string(handle_value));
        return 1;
    }

    LOG_INFO("Successfully adopted pipe handle: " + std::to_string(handle_value));

    // 3. 发送注册消息
    std::string register_msg = R"({"type":"register","process":"monitor"})";
    if (pipe.writeLine(register_msg) != PipeResult::OK) {
        LOG_ERROR("Failed to send registration message to launcher");
        return 1;
    }

    LOG_INFO("Registration message sent to launcher: " + register_msg);

    // 4. 主循环
    LOG_INFO("Monitor is ready, entering main loop...");

    int loop_count = 0;
    while (true) {
        ++loop_count;
        if (loop_count % 100 == 0) {
            LOG_INFO("Monitor main loop heartbeat: " + std::to_string(loop_count) + " iterations");
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
                LOG_INFO("Received from launcher: " + message);
                // TODO: 解析 launcher 发来的命令（如 REQUEST_ENGINE）
            } else if (read_result == PipeResult::BROKEN) {
                LOG_WARN("Pipe broken, exiting main loop");
                break;
            } else if (read_result == PipeResult::TIMEOUT) {
                continue;
            } else {
                LOG_WARN("readLine returned unexpected error: " +
                         std::to_string(static_cast<int>(read_result)));
                break;
            }
        } else if (peek_result == PipeResult::BROKEN) {
            LOG_WARN("Pipe broken, exiting main loop");
            break;
        } else if (peek_result == PipeResult::WOULD_BLOCK) {
            // 无数据，正常
        } else if (peek_result != PipeResult::OK) {
            LOG_WARN("peekAvailable returned unexpected error: " +
                     std::to_string(static_cast<int>(peek_result)));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // 5. 清理
    LOG_INFO("Monitor shutting down...");
    pipe.close();

    LOG_INFO("=== Monitor exited ===");
    return 0;
}