// src/core_engine/main.cpp
#include "logger.h"
#include "pipe.h"
#include "constants.h"

#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <tlhelp32.h>

using namespace dream_machine;

// ================================================================
// 内部辅助函数
// ================================================================
namespace {

std::string getArgValue(int argc, char* argv[], const std::string& key) {
    for (int i = 1; i < argc - 1; ++i) {
        if (argv[i] == key) {
            return argv[i + 1];
        }
    }
    return {};
}

bool verifyParentPid(DWORD expected_parent_pid) {
    if (expected_parent_pid == 0) {
        LOG_ERROR("Missing --parent-pid argument, refusing to run standalone");
        return false;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        LOG_ERROR("CreateToolhelp32Snapshot failed: error " + std::to_string(GetLastError()));
        return false;
    }

    PROCESSENTRY32W pe = {sizeof(PROCESSENTRY32W)};
    DWORD current_pid = GetCurrentProcessId();
    DWORD real_parent_pid = 0;

    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (pe.th32ProcessID == current_pid) {
                real_parent_pid = pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &pe));
    }

    CloseHandle(snapshot);

    if (real_parent_pid == 0) {
        LOG_ERROR("Failed to determine real parent PID");
        return false;
    }

    if (real_parent_pid != expected_parent_pid) {
        LOG_ERROR("Parent PID mismatch: expected " + std::to_string(expected_parent_pid) +
                  ", actual " + std::to_string(real_parent_pid) + ", refusing to run");
        return false;
    }

    LOG_INFO("Parent PID verification passed (PID: " + std::to_string(real_parent_pid) + ")");
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("core_engine");
    LOG_INFO("=== Dream Machine Core Engine starting ===");

    // ============================================================
    // 第一步：解析并验证启动参数（双重校验，防止独立启动）
    // ============================================================

    // 1.1 获取并验证父进程 PID（必须是 monitor）
    std::string parent_pid_str = getArgValue(argc, argv, "--parent-pid");
    DWORD expected_parent_pid = 0;
    if (!parent_pid_str.empty()) {
        expected_parent_pid = static_cast<DWORD>(std::stoul(parent_pid_str));
    }

    if (!verifyParentPid(expected_parent_pid)) {
        return 1;
    }

    // 1.2 获取并验证 session_id（必须非空）
    std::string session_id = getArgValue(argc, argv, "--session-id");
    if (session_id.empty()) {
        LOG_ERROR("Missing --session-id argument, refusing to run");
        return 1;
    }

    LOG_INFO("Session ID: " + session_id);

    // ============================================================
    // 第二步：连接到 executor（直接连接，无需重试）
    // ============================================================
    std::string executor_pipe_name_str = pipe_names::executor_core();
    std::wstring executor_pipe_name(executor_pipe_name_str.begin(),
                                     executor_pipe_name_str.end());

    LOG_INFO("Connecting to executor pipe: " + executor_pipe_name_str);

    NamedPipe executor_pipe;
    if (!executor_pipe.connect(executor_pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to executor pipe, exiting");
        return 1;
    }

    LOG_INFO("Connected to executor pipe");

    // ============================================================
    // 第三步：连接到 monitor（直接连接，无需重试）
    // ============================================================
    std::string monitor_pipe_name_str = pipe_names::monitor_core(session_id);
    std::wstring monitor_pipe_name(monitor_pipe_name_str.begin(),
                                    monitor_pipe_name_str.end());

    LOG_INFO("Connecting to monitor pipe: " + monitor_pipe_name_str);

    NamedPipe monitor_pipe;
    if (!monitor_pipe.connect(monitor_pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to monitor pipe, exiting");
        return 1;
    }

    LOG_INFO("Connected to monitor pipe");

    // ============================================================
    // 第四步：向 monitor 发送注册消息
    // ============================================================
    std::string register_msg = R"({"type":"register","cmd":"REGISTER_SESSION","payload":{"session_id":")" +
                               session_id + R"("}})";
    if (monitor_pipe.writeLine(register_msg) != PipeResult::PIPE_OK) {
        LOG_ERROR("Failed to send REGISTER_SESSION to monitor, exiting");
        return 1;
    }
    LOG_INFO("REGISTER_SESSION sent to monitor: " + register_msg);

    // ============================================================
    // 第五步：主循环（同时监控 monitor 和 executor）
    // ============================================================
    LOG_INFO("Entering main loop...");

    int heartbeat_counter = 0;
    constexpr int HEARTBEAT_INTERVAL = 100;

    while (true) {
        // 检查 monitor 管道
        if (monitor_pipe.isBroken()) {
            LOG_INFO("Monitor pipe broken, core_engine shutting down");
            break;
        }

        // 检查 executor 管道
        if (executor_pipe.isBroken()) {
            LOG_INFO("Executor pipe broken, core_engine shutting down");
            break;
        }

        // ---- 处理 monitor 消息 ----
        DWORD monitor_bytes = 0;
        PipeResult monitor_peek = monitor_pipe.peekAvailable(monitor_bytes);

        if (monitor_peek == PipeResult::PIPE_BROKEN) {
            LOG_INFO("Monitor pipe broken (peek), core_engine shutting down");
            break;
        }

        if (monitor_peek == PipeResult::PIPE_OK && monitor_bytes > 0) {
            std::string message;
            PipeResult read_result = monitor_pipe.readLine(message, 100);

            if (read_result == PipeResult::PIPE_OK) {
                LOG_INFO("From monitor: " + message);
                // TODO: 处理 SHUTDOWN 等命令
                if (message.find("\"SHUTDOWN\"") != std::string::npos) {
                    LOG_INFO("Received SHUTDOWN from monitor, exiting");
                    break;
                }
            } else if (read_result == PipeResult::PIPE_BROKEN) {
                LOG_INFO("Monitor pipe broken (read), core_engine shutting down");
                break;
            }
        }

        // ---- 处理 executor 消息 ----
        DWORD executor_bytes = 0;
        PipeResult executor_peek = executor_pipe.peekAvailable(executor_bytes);

        if (executor_peek == PipeResult::PIPE_BROKEN) {
            LOG_INFO("Executor pipe broken (peek), core_engine shutting down");
            break;
        }

        if (executor_peek == PipeResult::PIPE_OK && executor_bytes > 0) {
            std::string message;
            PipeResult read_result = executor_pipe.readLine(message, 100);

            if (read_result == PipeResult::PIPE_OK) {
                LOG_INFO("From executor: " + message);
                // TODO: 处理操作结果（STEP_*, OP_DONE, OP_ABORT）
            } else if (read_result == PipeResult::PIPE_BROKEN) {
                LOG_INFO("Executor pipe broken (read), core_engine shutting down");
                break;
            }
        }

        // ---- 心跳日志 ----
        ++heartbeat_counter;
        if (heartbeat_counter % HEARTBEAT_INTERVAL == 0) {
            LOG_INFO("Core engine heartbeat: " + std::to_string(heartbeat_counter) +
                     " iterations (session: " + session_id + ")");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // ============================================================
    // 第六步：向 monitor 发送注销消息（优雅退出）
    // ============================================================
    LOG_INFO("Sending UNREGISTER_SESSION to monitor...");
    std::string unregister_msg = R"({"type":"register","cmd":"UNREGISTER_SESSION","payload":{"session_id":")" +
                                 session_id + R"("}})";
    monitor_pipe.writeLine(unregister_msg);  // 尽力发送，忽略失败

    // ============================================================
    // 第七步：清理
    // ============================================================
    LOG_INFO("Shutting down core_engine...");
    monitor_pipe.close();
    executor_pipe.close();

    LOG_INFO("=== Core Engine exited (session: " + session_id + ") ===");
    return 0;
}