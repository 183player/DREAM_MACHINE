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

// ============================================================================
// 匿名命名空间
// ============================================================================

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

// ============================================================================
// 主入口
// ============================================================================

int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("monitor");
    LOG_INFO("=== Dream Machine Monitor starting ===");

    uintptr_t pipe_handle_value = 0;
    if (parsePipeHandleFromArgs(argc, argv, pipe_handle_value)) {
        LOG_INFO("Received --pipe-handle: " + std::to_string(pipe_handle_value));
    } else {
        LOG_INFO("No --pipe-handle provided, connecting via pipe name");
    }

    // 连接到 launcher 管道
    std::wstring pipe_name = L"\\\\.\\pipe\\DreamMachine_Launcher";
    NamedPipe pipe;

    LOG_INFO("Connecting to launcher pipe: " + std::string(pipe_name.begin(), pipe_name.end()));

    if (!pipe.connect(pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to launcher pipe");
        return 1;
    }

    LOG_INFO("Connected to launcher pipe");

    // 发送注册消息
    std::string register_msg = R"({"type":"register","process":"monitor"})";
    if (pipe.writeLine(register_msg) != PipeResult::OK) {
        LOG_ERROR("Failed to send registration message");
        return 1;
    }

    LOG_INFO("Registration message sent: " + register_msg);

    // 主循环
    LOG_INFO("Entering main loop...");

    // 增加计数器用于日志
    int loop_count = 0;

    while (true) {
        ++loop_count;
        if (loop_count % 100 == 0) {
            LOG_INFO("Monitor main loop heartbeat: " + std::to_string(loop_count) + " iterations");
        }

        // 先检查管道是否有效
        if (!pipe.isConnected()) {
            LOG_WARN("Monitor detected pipe is not connected, exiting main loop");
            break;
        }

        DWORD bytes_available = 0;
        PipeResult peek_result = pipe.peekAvailable(bytes_available);

        if (peek_result == PipeResult::OK && bytes_available > 0) {
            std::string message;
            PipeResult read_result = pipe.readLine(message, 3000);

            if (read_result == PipeResult::OK) {
                LOG_INFO("Received: " + message);
                // 根据消息类型处理（暂不回复，避免死循环）
                // TODO: 实现真正的消息路由
                // 这里只记录，不回复
            } else if (read_result == PipeResult::BROKEN) {
                LOG_WARN("readLine returned BROKEN, exiting main loop");
                break;
            } else if (read_result == PipeResult::TIMEOUT) {
                // 超时是正常的，继续循环
                continue;
            } else {
                LOG_WARN("readLine returned unexpected error: " + std::to_string(static_cast<int>(read_result)));
                break;
            }
        } else if (peek_result == PipeResult::BROKEN) {
            LOG_WARN("peekAvailable returned BROKEN, exiting main loop");
            break;
        } else if (peek_result == PipeResult::TIMEOUT) {
            // peek 超时？目前 peek 是非阻塞的，不会 TIMEOUT
            // 忽略
            continue;
        } else if (peek_result == PipeResult::WOULD_BLOCK) {
            // 无数据，正常
            // 什么也不做
        } else {
            LOG_WARN("peekAvailable returned unexpected error: " + std::to_string(static_cast<int>(peek_result)));
            // 不立即退出，因为可能是临时错误
        }

        // 非阻塞睡眠，避免 CPU 空转
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    LOG_INFO("Monitor main loop exited, shutting down...");
    pipe.close();
    LOG_INFO("=== Monitor exited ===");
    return 0;
}