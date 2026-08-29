// src/monitor/main.cpp
#include "logger.h"
#include "pipe.h"
#include "constants.h"
#include "process.h"

#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <vector>
#include <sstream>
#include <tlhelp32.h>   // 用于 CreateToolhelp32Snapshot 等

using namespace dream_machine;

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

// 预留：启动 core_engine（由 monitor 按需启动）
// 当前未使用，待后续实现 REQUEST_ENGINE 命令时激活
/* static bool launchCoreEngine(const std::string& session_id,
                      DWORD parent_pid,
                      HANDLE* out_process_handle = nullptr) {
    // 实现待后续完善
    return true;
} */

} // namespace

int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("monitor");
    LOG_INFO("=== Dream Machine Monitor starting ===");

    std::string parent_pid_str = getArgValue(argc, argv, "--parent-pid");
    DWORD expected_parent_pid = 0;
    if (!parent_pid_str.empty()) {
        expected_parent_pid = static_cast<DWORD>(std::stoul(parent_pid_str));
    }

    if (!verifyParentPid(expected_parent_pid)) {
        return 1;
    }

    std::string pipe_name_str = pipe_names::launcher_monitor();
    std::wstring pipe_name(pipe_name_str.begin(), pipe_name_str.end());

    LOG_INFO("Connecting to launcher pipe: " + pipe_name_str);

    NamedPipe pipe;
    if (!pipe.connect(pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to launcher pipe, exiting");
        return 1;
    }

    LOG_INFO("Connected to launcher pipe");

    std::string register_msg = R"({"type":"register","process":"monitor"})";
    if (pipe.writeLine(register_msg) != PipeResult::PIPE_OK) {
        LOG_ERROR("Failed to send registration message, exiting");
        return 1;
    }
    LOG_INFO("Registration message sent: " + register_msg);

    LOG_INFO("Entering main loop...");

    int heartbeat_counter = 0;
    constexpr int HEARTBEAT_INTERVAL = 100;

    while (true) {
        if (pipe.isBroken()) {
            LOG_INFO("Launcher pipe broken, monitor shutting down");
            break;
        }

        DWORD bytes_available = 0;
        PipeResult peek_result = pipe.peekAvailable(bytes_available);

        if (peek_result == PipeResult::PIPE_BROKEN) {
            LOG_INFO("Launcher pipe broken (peek), monitor shutting down");
            break;
        }

        if (peek_result == PipeResult::PIPE_OK && bytes_available > 0) {
            std::string message;
            PipeResult read_result = pipe.readLine(message, 100);

            if (read_result == PipeResult::PIPE_OK) {
                LOG_INFO("Received: " + message);
                // TODO: 处理 REQUEST_ENGINE 命令
            } else if (read_result == PipeResult::PIPE_BROKEN) {
                LOG_INFO("Launcher pipe broken (read), monitor shutting down");
                break;
            }
        }

        ++heartbeat_counter;
        if (heartbeat_counter % HEARTBEAT_INTERVAL == 0) {
            LOG_INFO("Monitor main loop heartbeat: " + std::to_string(heartbeat_counter) + " iterations");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    LOG_INFO("Shutting down monitor...");
    pipe.close();
    LOG_INFO("=== Monitor exited ===");
    return 0;
}