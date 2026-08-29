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

namespace {

struct SubprocessInfo {
    std::wstring name;
    std::wstring executable;
    std::wstring extra_args;
    std::string pipe_name;
};

bool launchSubprocess(const SubprocessInfo& info,
                      std::vector<Process>& managed_processes) {
    ProcessStartOptions options;
    options.executable = info.executable;
    options.args = info.extra_args;
    options.inherit_handles = true;

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
    if (!pipe.isValid()) {
        return;
    }

    DWORD bytes_available = 0;
    PipeResult peek_result = pipe.peekAvailable(bytes_available);

    if (peek_result == PipeResult::OK && bytes_available > 0) {
        std::string message;
        PipeResult read_result = pipe.readLine(message, 100);

        if (read_result == PipeResult::OK) {
            LOG_INFO("[" + name + "] Received: " + message);
        } else if (read_result == PipeResult::BROKEN) {
            LOG_WARN("[" + name + "] Pipe broken");
        }
    } else if (peek_result == PipeResult::BROKEN) {
        LOG_WARN("[" + name + "] Peek failed: pipe broken");
    }
}

} // namespace

int main() {
    Logger::instance().setProcessName("launcher");
    LOG_INFO("=== Dream Machine Launcher starting ===");

    // ============================================================
    // 创建三个独立的命名管道服务端（名称约定）
    // 专家模式结论：Windows 命名管道基于名称连接，句柄传递不可行
    // ============================================================

    constexpr int MAX_INSTANCES = 1;

    std::string monitor_pipe_name_str = pipe_names::launcher_monitor();
    std::string executor_pipe_name_str = pipe_names::launcher_executor();
    std::string gui_pipe_name_str = pipe_names::launcher_gui();

    std::wstring monitor_pipe_name(monitor_pipe_name_str.begin(), monitor_pipe_name_str.end());
    std::wstring executor_pipe_name(executor_pipe_name_str.begin(), executor_pipe_name_str.end());
    std::wstring gui_pipe_name(gui_pipe_name_str.begin(), gui_pipe_name_str.end());

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

    // 启动子进程
    std::vector<Process> managed_processes;

    std::vector<SubprocessInfo> subprocesses;
    subprocesses.push_back({L"monitor", L"monitor.exe", L"", monitor_pipe_name_str});
    subprocesses.push_back({L"executor", L"executor.exe", L"", executor_pipe_name_str});
    subprocesses.push_back({L"gui", L"gui.exe", L"", gui_pipe_name_str});

    for (const auto& info : subprocesses) {
        if (!launchSubprocess(info, managed_processes)) {
            LOG_ERROR("Failed to launch " + std::string(info.name.begin(), info.name.end()));
        }
    }

    // 等待子进程连接
    LOG_INFO("Waiting for subprocesses to connect...");

    int connected_count = 0;

    if (monitor_pipe.waitForClient(15000) == PipeResult::OK) {
        ++connected_count;
        LOG_INFO("monitor connected (1/3)");
    } else {
        LOG_WARN("monitor connection timeout or failed");
    }

    if (executor_pipe.waitForClient(15000) == PipeResult::OK) {
        ++connected_count;
        LOG_INFO("executor connected (2/3)");
    } else {
        LOG_WARN("executor connection timeout or failed");
    }

    if (gui_pipe.waitForClient(15000) == PipeResult::OK) {
        ++connected_count;
        LOG_INFO("gui connected (3/3)");
    } else {
        LOG_WARN("gui connection timeout or failed");
    }

    if (connected_count < 3) {
        LOG_WARN("Only " + std::to_string(connected_count) + "/3 subprocesses connected");
    } else {
        LOG_INFO("All subprocesses connected successfully");
    }

    // 主循环
    LOG_INFO("Entering main loop...");

    while (true) {
        bool monitor_connected = monitor_pipe.isConnected();
        bool executor_connected = executor_pipe.isConnected();
        bool gui_connected = gui_pipe.isConnected();

        if (!monitor_connected || !executor_connected || !gui_connected) {
            LOG_INFO("A subprocess pipe disconnected, launcher shutting down");
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

        pollPipe(monitor_pipe, "monitor");
        pollPipe(executor_pipe, "executor");
        pollPipe(gui_pipe, "gui");

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 清理
    LOG_INFO("Shutting down launcher...");

    monitor_pipe.close();
    executor_pipe.close();
    gui_pipe.close();

    for (auto& proc : managed_processes) {
        if (proc.isRunning()) {
            LOG_INFO("Terminating process (PID: " + std::to_string(proc.getPid()) + ")...");
            proc.terminate();
        }
    }

    LOG_INFO("=== Launcher exited ===");
    return 0;
}