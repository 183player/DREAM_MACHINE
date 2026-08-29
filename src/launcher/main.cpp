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
    // extra_args 不再使用，改为由 launchSubprocess 动态构造
};

bool launchSubprocess(const SubprocessInfo& info,
                      std::vector<Process>& managed_processes,
                      HANDLE job_handle,
                      DWORD parent_pid) {
    ProcessStartOptions options;
    options.executable = info.executable;

    // 构造命令行参数：传递父进程 PID，供子进程校验
    std::wstring args = L"--parent-pid " + std::to_wstring(parent_pid);
    options.args = args;

    options.inherit_handles = true;
    options.job_handle = job_handle;
    options.creation_flags = ProcessCreationFlags::PROC_NO_WINDOW;
    options.timeout_ms = 2000;

    Process proc;
    if (!proc.start(options)) {
        LOG_ERROR("Failed to launch " + std::string(info.name.begin(), info.name.end()));
        return false;
    }

    managed_processes.push_back(std::move(proc));
    LOG_INFO("Launched " + std::string(info.name.begin(), info.name.end()) +
             " (PID: " + std::to_string(managed_processes.back().getPid()) +
             ", attached to Job Object)");
    return true;
}

void pollPipe(NamedPipe& pipe, const std::string& name, bool& out_broken) {
    out_broken = false;

    if (!pipe.isValid()) {
        out_broken = true;
        return;
    }

    if (pipe.isBroken()) {
        LOG_WARN("[" + name + "] Pipe broken (isBroken)");
        out_broken = true;
        return;
    }

    DWORD bytes_available = 0;
    PipeResult peek_result = pipe.peekAvailable(bytes_available);

    if (peek_result == PipeResult::PIPE_BROKEN) {
        LOG_WARN("[" + name + "] Pipe broken (peek failed)");
        out_broken = true;
        return;
    }

    if (peek_result != PipeResult::PIPE_OK) {
        return;
    }

    if (bytes_available > 0) {
        std::string message;
        PipeResult read_result = pipe.readLine(message, 100);

        if (read_result == PipeResult::PIPE_OK) {
            LOG_INFO("[" + name + "] Received: " + message);
        } else if (read_result == PipeResult::PIPE_BROKEN) {
            LOG_WARN("[" + name + "] Pipe broken (read failed)");
            out_broken = true;
        }
    }
}

void cleanup(const std::vector<Process>& managed_processes, HANDLE job_handle) {
    LOG_INFO("Shutting down launcher...");

    for (const auto& proc : managed_processes) {
        if (proc.isRunning()) {
            LOG_INFO("Terminating process (PID: " + std::to_string(proc.getPid()) + ")...");
            proc.terminate();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } else {
            LOG_INFO("Process (PID: " + std::to_string(proc.getPid()) + ") already exited");
        }
    }

    if (job_handle && job_handle != INVALID_HANDLE_VALUE) {
        LOG_INFO("Closing Job Object (KILL_ON_JOB_CLOSE will terminate any remaining processes)...");
        CloseHandle(job_handle);
        job_handle = nullptr;
    }

    LOG_INFO("Cleanup complete");
}

} // namespace

int main() {
    Logger::instance().setProcessName("launcher");
    LOG_INFO("=== Dream Machine Launcher starting ===");

    std::vector<Process> managed_processes;

    HANDLE job_handle = CreateJobObjectW(nullptr, L"Global\\DreamMachine_Launcher_Job");
    if (!job_handle) {
        LOG_ERROR("Failed to create Job Object: error " + std::to_string(GetLastError()));
    } else {
        LOG_INFO("Job Object created successfully");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
        job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation,
                                     &job_info, sizeof(job_info))) {
            LOG_ERROR("Failed to configure Job Object: error " + std::to_string(GetLastError()));
        } else {
            LOG_INFO("Job Object configured: KILL_ON_JOB_CLOSE enabled");
        }
    }

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

    if (!monitor_pipe.createServer(monitor_pipe_name, MAX_INSTANCES, true)) {
        LOG_ERROR("Failed to create monitor pipe server");
        cleanup(managed_processes, job_handle);
        return 1;
    }

    if (!executor_pipe.createServer(executor_pipe_name, MAX_INSTANCES, true)) {
        LOG_ERROR("Failed to create executor pipe server");
        cleanup(managed_processes, job_handle);
        return 1;
    }

    if (!gui_pipe.createServer(gui_pipe_name, MAX_INSTANCES, true)) {
        LOG_ERROR("Failed to create gui pipe server");
        cleanup(managed_processes, job_handle);
        return 1;
    }

    LOG_INFO("All three pipe servers created successfully (secure mode enabled)");

    std::vector<SubprocessInfo> subprocesses;
    subprocesses.push_back({L"monitor", L"monitor.exe"});
    subprocesses.push_back({L"executor", L"executor.exe"});
    subprocesses.push_back({L"gui", L"gui.exe"});

    DWORD parent_pid = GetCurrentProcessId();
    for (const auto& info : subprocesses) {
        if (!launchSubprocess(info, managed_processes, job_handle, parent_pid)) {
            LOG_ERROR("Failed to launch " + std::string(info.name.begin(), info.name.end()));
        }
    }

    if (managed_processes.size() != 3) {
        LOG_WARN("Only " + std::to_string(managed_processes.size()) +
                 "/3 subprocesses started successfully");
    }

    LOG_INFO("Waiting for subprocesses to connect...");

    int connected_count = 0;

    PipeResult monitor_connect = monitor_pipe.waitForClient(15000);
    if (monitor_connect == PipeResult::PIPE_OK) {
        ++connected_count;
        LOG_INFO("monitor connected (1/3)");
    } else {
        LOG_WARN("monitor connection timeout or failed");
    }

    PipeResult executor_connect = executor_pipe.waitForClient(15000);
    if (executor_connect == PipeResult::PIPE_OK) {
        ++connected_count;
        LOG_INFO("executor connected (2/3)");
    } else {
        LOG_WARN("executor connection timeout or failed");
    }

    PipeResult gui_connect = gui_pipe.waitForClient(15000);
    if (gui_connect == PipeResult::PIPE_OK) {
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

    LOG_INFO("Entering main loop...");

    while (true) {
        bool monitor_broken = false;
        bool executor_broken = false;
        bool gui_broken = false;

        pollPipe(monitor_pipe, "monitor", monitor_broken);
        pollPipe(executor_pipe, "executor", executor_broken);
        pollPipe(gui_pipe, "gui", gui_broken);

        if (monitor_broken || executor_broken || gui_broken) {
            LOG_INFO("A subprocess pipe disconnected, launcher shutting down");
            break;
        }

        bool all_alive = true;
        for (const auto& proc : managed_processes) {
            if (!proc.isRunning()) {
                LOG_INFO("Subprocess (PID: " + std::to_string(proc.getPid()) + ") has exited");
                all_alive = false;
                break;
            }
        }

        if (!all_alive) {
            LOG_INFO("All subprocesses have exited, launcher shutting down");
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    monitor_pipe.close();
    executor_pipe.close();
    gui_pipe.close();

    cleanup(managed_processes, job_handle);

    LOG_INFO("=== Launcher exited ===");
    return 0;
}