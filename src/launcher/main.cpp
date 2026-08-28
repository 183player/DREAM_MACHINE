// src/launcher/main.cpp
#include "logger.h"
#include "pipe.h"
#include "process.h"
#include "constants.h"

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

using namespace dream_machine;

// ============================================================================
// 匿名命名空间：内部链接
// ============================================================================

namespace {

struct SubprocessInfo {
    std::wstring name;
    std::wstring executable;
    std::wstring extra_args;
};

bool launchSubprocess(const SubprocessInfo& info,
                      std::vector<Process>& managed_processes) {
    ProcessStartOptions options;
    options.executable = info.executable;
    options.args = info.extra_args;
    options.inherit_handles = true;
    options.pipe_handle = 0;

    Process proc;
    if (!proc.start(options)) {
        LOG_ERROR("Failed to launch " + std::string(info.name.begin(), info.name.end()));
        return false;
    }

    managed_processes.push_back(std::move(proc));
    LOG_INFO("Launched " + std::string(info.name.begin(), info.name.end()) +
             " (PID: " + std::to_string(managed_processes.back().getPid()) + ")");
    return true;
}

void pollPipe(NamedPipe& pipe, const std::string& name) {
    DWORD bytes_available = 0;
    PipeResult peek_result = pipe.peekAvailable(bytes_available);

    if (peek_result == PipeResult::OK && bytes_available > 0) {
        std::string message;
        PipeResult read_result = pipe.readLine(message, 100);

        if (read_result == PipeResult::OK) {
            LOG_INFO("[" + name + "] Received: " + message);
        } else if (read_result == PipeResult::BROKEN) {
            LOG_WARN("[" + name + "] Pipe broken");
        } else if (read_result == PipeResult::TIMEOUT) {
            // 超时忽略
        } else {
            LOG_WARN("[" + name + "] Read error: " + std::to_string(static_cast<int>(read_result)));
        }
    } else if (peek_result == PipeResult::BROKEN) {
        LOG_WARN("[" + name + "] Peek failed: pipe broken");
    }
}

} // namespace

// ============================================================================
// 主入口
// ============================================================================

int main() {
    // 1. 初始化日志
    Logger::instance().setProcessName("launcher");
    LOG_INFO("=== Dream Machine Launcher starting ===");

    // 2. 创建三个独立的命名管道服务端实例
    //    每个子进程使用独立的管道名
    std::string monitor_pipe_name_str = pipe_names::launcher_monitor();
    std::string executor_pipe_name_str = pipe_names::launcher_executor();
    std::string gui_pipe_name_str = pipe_names::launcher_gui();

    std::wstring monitor_pipe_name(monitor_pipe_name_str.begin(), monitor_pipe_name_str.end());
    std::wstring executor_pipe_name(executor_pipe_name_str.begin(), executor_pipe_name_str.end());
    std::wstring gui_pipe_name(gui_pipe_name_str.begin(), gui_pipe_name_str.end());

    constexpr int MAX_INSTANCES = 1;

    NamedPipe monitor_pipe;
    NamedPipe executor_pipe;
    NamedPipe gui_pipe;

    LOG_INFO("Creating three pipe instances for monitor, executor, gui...");

    if (!monitor_pipe.createServer(monitor_pipe_name, MAX_INSTANCES)) {
        LOG_ERROR("Failed to create monitor pipe server");
        return 1;
    }

    if (!executor_pipe.createServer(executor_pipe_name, MAX_INSTANCES)) {
        LOG_ERROR("Failed to create executor pipe server");
        return 1;
    }

    if (!gui_pipe.createServer(gui_pipe_name, MAX_INSTANCES)) {
        LOG_ERROR("Failed to create gui pipe server");
        return 1;
    }

    LOG_INFO("All three pipe servers created successfully");

    // 3. 启动子进程
    std::vector<Process> managed_processes;

    std::vector<SubprocessInfo> subprocesses;
    subprocesses.push_back(SubprocessInfo{L"monitor", L"monitor.exe", L""});
    subprocesses.push_back(SubprocessInfo{L"executor", L"executor.exe", L""});
    subprocesses.push_back(SubprocessInfo{L"gui", L"gui.exe", L""});

    for (const auto& info : subprocesses) {
        if (!launchSubprocess(info, managed_processes)) {
            LOG_ERROR("Failed to launch " + std::string(info.name.begin(), info.name.end()));
        }
    }

    // 4. 分别等待三个子进程连接
    LOG_INFO("Waiting for subprocesses to connect...");

    const int expected_connections = static_cast<int>(subprocesses.size());
    int connected_count = 0;

    if (monitor_pipe.waitForClient(15000) == PipeResult::OK) {
        ++connected_count;
        LOG_INFO("monitor connected (" + std::to_string(connected_count) + "/" +
                 std::to_string(expected_connections) + ")");
    } else {
        LOG_WARN("monitor connection timeout or failed");
    }

    if (executor_pipe.waitForClient(15000) == PipeResult::OK) {
        ++connected_count;
        LOG_INFO("executor connected (" + std::to_string(connected_count) + "/" +
                 std::to_string(expected_connections) + ")");
    } else {
        LOG_WARN("executor connection timeout or failed");
    }

    if (gui_pipe.waitForClient(15000) == PipeResult::OK) {
        ++connected_count;
        LOG_INFO("gui connected (" + std::to_string(connected_count) + "/" +
                 std::to_string(expected_connections) + ")");
    } else {
        LOG_WARN("gui connection timeout or failed");
    }

    if (connected_count < expected_connections) {
        LOG_WARN("Only " + std::to_string(connected_count) + "/" +
                 std::to_string(expected_connections) + " subprocesses connected");
    } else {
        LOG_INFO("All subprocesses connected successfully");
    }

    // 5. 主循环
    LOG_INFO("Entering main loop...");

    while (true) {
        // 先检查 GUI 管道是否断开（用户关闭窗口）
        if (!gui_pipe.isConnected()) {
            LOG_INFO("GUI pipe disconnected, launcher shutting down");
            break;
        }

        pollPipe(monitor_pipe, "monitor");
        pollPipe(executor_pipe, "executor");
        pollPipe(gui_pipe, "gui");

        // 检查各管道连接状态（备用逻辑）
        if (!monitor_pipe.isConnected() && !executor_pipe.isConnected() && !gui_pipe.isConnected()) {
            LOG_INFO("All pipes are broken, launcher shutting down");
            break;
        }

        bool any_alive = false;
        for (const auto& proc : managed_processes) {
            if (proc.isRunning()) {
                any_alive = true;
                break;
            }
        }

        if (!any_alive) {
            LOG_INFO("All subprocesses have exited, launcher shutting down");
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 6. 清理
    LOG_INFO("Shutting down launcher...");

    monitor_pipe.close();
    executor_pipe.close();
    gui_pipe.close();

    for (auto& proc : managed_processes) {
        if (proc.isRunning()) {
            LOG_INFO("Waiting for process (PID: " + std::to_string(proc.getPid()) + ") to exit...");
            proc.waitForExit(3000);
            if (proc.isRunning()) {
                LOG_WARN("Process (PID: " + std::to_string(proc.getPid()) + ") still running, terminating...");
                proc.terminate();
            }
        }
    }

    LOG_INFO("=== Launcher exited ===");
    return 0;
}