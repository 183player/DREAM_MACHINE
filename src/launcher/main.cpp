// src/launcher/main.cpp
#include "logger.h"
#include "pipe.h"
#include "process.h"
#include "constants.h"
#include "plugin_manager.h"
#include "messages.h"

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <iomanip>

using namespace dream_machine;
using namespace dream_machine::launcher;

// ================================================================
// 内部辅助（匿名命名空间）
// ================================================================
namespace {

struct SubprocessInfo {
    std::wstring name;
    std::wstring executable;
};

// 会话运行状态（内部链接）
struct SessionState {
    std::string session_id;
    std::string state;          // "running", "terminated", "crashed"
    std::string pipe_name;
    DWORD pid = 0;
};

// 全局状态
std::unordered_map<std::string, SessionState> sessions_;
std::mutex sessions_mutex_;
std::unique_ptr<PluginManager> g_plugin_manager;

bool launchSubprocess(const SubprocessInfo& info,
                      std::vector<Process>& managed_processes,
                      HANDLE job_handle,
                      DWORD parent_pid) {
    ProcessStartOptions options;
    options.executable = info.executable;
    options.args = L"--parent-pid " + std::to_wstring(parent_pid);
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

            // ---- 解析并处理消息 ----
            std::string type, cmd, payload;
            if (parseBaseMessage(message, type, cmd, payload)) {
                // 处理插件管理相关命令（来自 gui）
                if (type == msg_types::PLUGIN_IMPORT) {
                    auto import_msg = parsePluginImport(payload);
                    if (import_msg.has_value()) {
                        std::string plugin_id;
                        bool success = g_plugin_manager->importPlugin(import_msg->package_path, plugin_id);
                        PluginImportRespMessage resp;
                        resp.success = success;
                        if (success) {
                            resp.plugin_id = plugin_id;
                            LOG_INFO("Plugin imported: " + plugin_id);
                        } else {
                            resp.error = "Import failed";
                        }
                        std::string resp_json = serializePluginImportResp(resp);
                        pipe.writeLine(resp_json);
                    }
                }
                else if (type == msg_types::PLUGIN_DELETE) {
                    auto delete_msg = parsePluginDelete(payload);
                    if (delete_msg.has_value()) {
                        bool success = g_plugin_manager->deletePlugin(delete_msg->plugin_id);
                        PluginDeleteRespMessage resp;
                        resp.success = success;
                        if (!success) {
                            resp.error = "Delete failed";
                        }
                        std::string resp_json = serializePluginDeleteResp(resp);
                        pipe.writeLine(resp_json);
                    }
                }
                else if (type == msg_types::PLUGIN_ENABLE) {
                    auto enable_msg = parsePluginEnable(payload);
                    if (enable_msg.has_value()) {
                        bool success = g_plugin_manager->setPluginEnabled(enable_msg->plugin_id,
                                                                         enable_msg->enabled);
                        PluginEnableRespMessage resp;
                        resp.success = success;
                        if (!success) {
                            resp.error = "Enable/disable failed";
                        }
                        std::string resp_json = serializePluginEnableResp(resp);
                        pipe.writeLine(resp_json);
                    }
                }
                // 其他消息（如会话状态等）由其他模块处理
            }

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

// 处理 monitor 发来的会话状态变更
void handleSessionStateChange(const std::string& message, NamedPipe& gui_pipe) {
    std::string session_id;
    std::string state;
    std::string pipe_name;

    size_t id_pos = message.find("\"session_id\"");
    if (id_pos != std::string::npos) {
        size_t start = message.find('\"', id_pos + 14);
        if (start != std::string::npos) {
            size_t end = message.find('\"', start + 1);
            if (end != std::string::npos) {
                session_id = message.substr(start + 1, end - start - 1);
            }
        }
    }

    size_t state_pos = message.find("\"state\"");
    if (state_pos != std::string::npos) {
        size_t start = message.find('\"', state_pos + 8);
        if (start != std::string::npos) {
            size_t end = message.find('\"', start + 1);
            if (end != std::string::npos) {
                state = message.substr(start + 1, end - start - 1);
            }
        }
    }

    size_t pipe_pos = message.find("\"pipe_name\"");
    if (pipe_pos != std::string::npos) {
        size_t start = message.find('\"', pipe_pos + 12);
        if (start != std::string::npos) {
            size_t end = message.find('\"', start + 1);
            if (end != std::string::npos) {
                pipe_name = message.substr(start + 1, end - start - 1);
            }
        }
    }

    if (session_id.empty() || state.empty()) {
        LOG_WARN("Invalid SESSION_STATE_CHANGED message");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            SessionState new_state;
            new_state.session_id = session_id;
            new_state.state = state;
            new_state.pipe_name = pipe_name;
            sessions_[session_id] = new_state;
        } else {
            it->second.state = state;
            if (!pipe_name.empty()) {
                it->second.pipe_name = pipe_name;
            }
        }
    }

    LOG_INFO("Session state updated: " + session_id + " -> " + state);

    if (gui_pipe.isValid() && gui_pipe.isConnected()) {
        auto update_msg = SessionStateUpdateMessage{session_id, state};
        std::string gui_msg = serializeSessionStateUpdate(update_msg);
        gui_pipe.writeLine(gui_msg);
        LOG_INFO("Forwarded session state to gui: " + session_id + " -> " + state);
    }
}

void handleActiveSessionsResp(const std::string& message, NamedPipe& gui_pipe) {
    if (gui_pipe.isValid() && gui_pipe.isConnected()) {
        gui_pipe.writeLine(message);
        LOG_INFO("Forwarded ACTIVE_SESSIONS_RESP to gui");
    }
}

// ================================================================
// 诊断功能：显示插件信息
// ================================================================
int showPluginInfo() {
    PluginManager pm;

    std::cout << "\n========== Dream Machine Plugin Info ==========\n" << std::endl;

    // 扫描插件
    if (!pm.scanPlugins()) {
        std::cerr << "Failed to scan plugins." << std::endl;
        return 1;
    }

    auto manifests = pm.getLoadedManifests();

    std::cout << "Total plugins: " << manifests.size() << "\n" << std::endl;

    // 系统插件
    auto system_ids = pm.getSystemPluginIds();
    std::cout << "System plugins (" << system_ids.size() << "):" << std::endl;
    for (const auto& id : system_ids) {
        auto it = manifests.find(id);
        if (it != manifests.end()) {
            const auto& m = it->second;
            std::cout << "  - " << m.id << " (v" << m.version << ")"
                      << " [enabled: " << (m.enabled ? "yes" : "no") << "]"
                      << " [sequence: " << m.sequence << "]"
                      << std::endl;
        }
    }

    // 用户插件
    auto user_ids = pm.getUserPluginIds();
    std::cout << "\nUser plugins (" << user_ids.size() << "):" << std::endl;
    for (const auto& id : user_ids) {
        auto it = manifests.find(id);
        if (it != manifests.end()) {
            const auto& m = it->second;
            std::cout << "  - " << m.id << " (v" << m.version << ")"
                      << " [enabled: " << (m.enabled ? "yes" : "no") << "]"
                      << " [sequence: " << m.sequence << "]"
                      << std::endl;
        }
    }

    // 检查系统插件备份
    std::cout << "\nBackup status: "
              << (PluginManager::hasSystemPluginBackup() ? "available" : "not found")
              << std::endl;

    std::cout << "\n================================================\n" << std::endl;

    return 0;
}

} // namespace

// ================================================================
// main 入口
// ================================================================
int main(int argc, char* argv[]) {
    // ============================================================
    // 诊断模式：--show-plugin-info
    // ============================================================
    if (argc > 1 && std::string(argv[1]) == "--show-plugin-info") {
        return showPluginInfo();
    }

    Logger::instance().setProcessName("launcher");
    LOG_INFO("=== Dream Machine Launcher starting ===");

    // ============================================================
    // 初始化插件管理器
    // ============================================================
    g_plugin_manager = std::make_unique<PluginManager>();

    // 验证系统插件完整性
    if (!g_plugin_manager->verifySystemPlugins()) {
        LOG_WARN("System plugin integrity check failed, continuing anyway");
    }

    // 扫描所有插件
    if (!g_plugin_manager->scanPlugins()) {
        LOG_WARN("Plugin scan failed, continuing without plugins");
    }

    // 生成初始化列表（稍后分发）
    plugin::InitList init_list = g_plugin_manager->generateInitList();

    std::vector<Process> managed_processes;

    // ============================================================
    // 创建 Job Object（含 SILENT_BREAKAWAY_OK）
    // ============================================================
    HANDLE job_handle = CreateJobObjectW(nullptr, L"Global\\DreamMachine_Launcher_Job");
    if (!job_handle) {
        LOG_ERROR("Failed to create Job Object: error " + std::to_string(GetLastError()));
    } else {
        LOG_INFO("Job Object created successfully");

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
        job_info.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
            JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;

        if (!SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation,
                                     &job_info, sizeof(job_info))) {
            LOG_ERROR("Failed to configure Job Object: error " + std::to_string(GetLastError()));
        } else {
            LOG_INFO("Job Object configured: KILL_ON_JOB_CLOSE + SILENT_BREAKAWAY_OK enabled");
        }
    }

    // ============================================================
    // 创建三个管道服务端
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

    // ============================================================
    // 启动子进程
    // ============================================================
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

    // ============================================================
    // 等待子进程连接
    // ============================================================
    LOG_INFO("Waiting for subprocesses to connect...");

    int connected_count = 0;

    if (monitor_pipe.waitForClient(15000) == PipeResult::PIPE_OK) {
        ++connected_count;
        LOG_INFO("monitor connected (1/3)");
    } else {
        LOG_WARN("monitor connection timeout or failed");
    }

    if (executor_pipe.waitForClient(15000) == PipeResult::PIPE_OK) {
        ++connected_count;
        LOG_INFO("executor connected (2/3)");
    } else {
        LOG_WARN("executor connection timeout or failed");
    }

    if (gui_pipe.waitForClient(15000) == PipeResult::PIPE_OK) {
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

    // ============================================================
    // 发送初始化列表给所有进程
    // ============================================================
    if (connected_count == 3) {
        bool dist_ok = g_plugin_manager->distributeInitList(gui_pipe, executor_pipe, monitor_pipe, init_list);
        if (dist_ok) {
            LOG_INFO("INIT_LIST distributed to all processes");
        } else {
            LOG_WARN("INIT_LIST distribution incomplete");
        }

        // 向 gui 发送初始会话列表（空，后续由 monitor 推送更新）
        std::string init_list_msg;
        init_list_msg += R"({"type":"session_state","cmd":"INIT_SESSION_LIST","payload":{"sessions":[]}})";
        if (gui_pipe.isValid() && gui_pipe.isConnected()) {
            gui_pipe.writeLine(init_list_msg);
            LOG_INFO("Sent INIT_SESSION_LIST to gui");
        }
    } else {
        LOG_WARN("Not all processes connected, skipping INIT_LIST distribution");
    }

    // ============================================================
    // 主循环
    // ============================================================
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

    // ============================================================
    // 清理
    // ============================================================
    monitor_pipe.close();
    executor_pipe.close();
    gui_pipe.close();

    cleanup(managed_processes, job_handle);

    // 释放插件管理器
    g_plugin_manager.reset();

    LOG_INFO("=== Launcher exited ===");
    return 0;
}