// src/launcher/main.cpp
#include "logger.h"
#include "pipe.h"
#include "process.h"
#include "constants.h"
#include "plugin_manager.h"
#include "messages.h"
#include "error_codes.h"

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <iomanip>
#include <atomic>

using namespace dream_machine;
using namespace dream_machine::launcher;

// ================================================================
// 前向声明（解决循环调用问题）
// ================================================================
namespace {
    void handleFullSyncResponse(const std::string& payload);
    void requestFullSync(NamedPipe& monitor_pipe);
    void onFullSyncTimer(NamedPipe& monitor_pipe);
}

// ================================================================
// 内部辅助（匿名命名空间）
// ================================================================
namespace {

struct SubprocessInfo {
    std::wstring name;
    std::wstring executable;
};

struct SessionState {
    std::string session_id;
    std::string state;
    std::string pipe_name;
    DWORD pid = 0;
    uint64_t last_update = 0;
};

std::unordered_map<std::string, SessionState> sessions_;
std::mutex sessions_mutex_;
std::unique_ptr<PluginManager> g_plugin_manager;
std::atomic<bool> g_full_sync_in_progress{false};
uint64_t g_full_sync_sequence = 0;

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

            std::string type, cmd, payload;
            if (parseBaseMessage(message, type, cmd, payload)) {
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
                else if (type == msg_types::FULL_SYNC_RESPONSE) {
                    handleFullSyncResponse(payload);
                }
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

// ================================================================
// 会话状态处理
// ================================================================
void handleSessionStateChange(const std::string& payload, NamedPipe& gui_pipe) {
    auto msg = parseSessionStateChanged(payload);
    if (!msg.has_value()) {
        LOG_WARN("Failed to parse SESSION_STATE_CHANGED message");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(msg->session_id);
        if (it == sessions_.end()) {
            SessionState new_state;
            new_state.session_id = msg->session_id;
            new_state.state = msg->state;
            new_state.pipe_name = msg->pipe_name.value_or("");
            new_state.last_update = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            sessions_[msg->session_id] = new_state;
        } else {
            it->second.state = msg->state;
            if (msg->pipe_name.has_value()) {
                it->second.pipe_name = *msg->pipe_name;
            }
            it->second.last_update = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
        }
    }

    LOG_INFO("Session state updated: " + msg->session_id + " -> " + msg->state);

    if (gui_pipe.isValid() && gui_pipe.isConnected()) {
        SessionStateUpdateMessage update_msg;
        update_msg.session_id = msg->session_id;
        update_msg.state = msg->state;
        std::string gui_msg = serializeSessionStateUpdate(update_msg);
        gui_pipe.writeLine(gui_msg);
        LOG_INFO("Forwarded session state to gui: " + msg->session_id + " -> " + msg->state);
    }
}

void handleActiveSessionsResp(const std::string& message, NamedPipe& gui_pipe) {
    if (gui_pipe.isValid() && gui_pipe.isConnected()) {
        gui_pipe.writeLine(message);
        LOG_INFO("Forwarded ACTIVE_SESSIONS_RESP to gui");
    }
}

// ================================================================
// 全量同步机制
// ================================================================

void requestFullSync(NamedPipe& monitor_pipe) {
    if (!monitor_pipe.isValid() || !monitor_pipe.isConnected()) {
        LOG_WARN("Cannot request full sync: monitor pipe not connected");
        return;
    }

    if (g_full_sync_in_progress.exchange(true)) {
        LOG_WARN("Full sync already in progress, skipping");
        return;
    }

    ++g_full_sync_sequence;
    FullSyncRequestMessage req;
    req.request_id = static_cast<int64_t>(g_full_sync_sequence);
    std::string req_json = serializeFullSyncRequest(req);

    if (monitor_pipe.writeLine(req_json) == PipeResult::PIPE_OK) {
        LOG_INFO("Full sync request sent (seq: " + std::to_string(g_full_sync_sequence) + ")");
    } else {
        LOG_ERROR("Failed to send full sync request");
        g_full_sync_in_progress = false;
    }
}

void handleFullSyncResponse(const std::string& payload) {
    auto resp = parseFullSyncResponse(payload);
    if (!resp.has_value()) {
        LOG_WARN("Failed to parse FULL_SYNC_RESPONSE");
        g_full_sync_in_progress = false;
        return;
    }

    LOG_INFO("Full sync response received (request_id: " + std::to_string(resp->request_id) +
             ", sessions: " + std::to_string(resp->sessions.size()) + ")");

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.clear();

        for (const auto& s : resp->sessions) {
            SessionState state;
            state.session_id = s.session_id;
            state.state = s.state;
            state.pipe_name = s.pipe_name.value_or("");
            state.last_update = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            sessions_[s.session_id] = state;
            LOG_INFO("Sync: session " + s.session_id + " -> " + s.state);
        }
    }

    g_full_sync_in_progress = false;
}

void onFullSyncTimer(NamedPipe& monitor_pipe) {
    if (!monitor_pipe.isValid() || !monitor_pipe.isConnected()) {
        return;
    }
    LOG_INFO("Periodic full sync triggered");
    requestFullSync(monitor_pipe);
}

// ================================================================
// 诊断功能：显示插件信息
// ================================================================
int showPluginInfo() {
    PluginManager pm;

    std::cout << "\n========== Dream Machine Plugin Info ==========\n" << std::endl;

    if (!pm.scanPlugins()) {
        std::cerr << "Failed to scan plugins." << std::endl;
        return 1;
    }

    auto manifests = pm.getLoadedManifests();

    std::cout << "Total plugins: " << manifests.size() << "\n" << std::endl;

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
    if (argc > 1 && std::string(argv[1]) == "--show-plugin-info") {
        return showPluginInfo();
    }

    Logger::instance().setProcessName("launcher");
    LOG_INFO("=== Dream Machine Launcher starting ===");

    g_plugin_manager = std::make_unique<PluginManager>();

    if (!g_plugin_manager->verifySystemPlugins()) {
        LOG_WARN("System plugin integrity check failed, continuing anyway");
    }

    if (!g_plugin_manager->scanPlugins()) {
        LOG_WARN("Plugin scan failed, continuing without plugins");
    }

    plugin::InitList init_list = g_plugin_manager->generateInitList();

    std::vector<Process> managed_processes;

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

    if (connected_count == 3) {
        bool dist_ok = g_plugin_manager->distributeInitList(gui_pipe, executor_pipe, monitor_pipe, init_list);
        if (dist_ok) {
            LOG_INFO("INIT_LIST distributed to all processes");
        } else {
            LOG_WARN("INIT_LIST distribution incomplete");
        }

        InitSessionListMessage init_session_msg;
        std::string init_session_str = serializeInitSessionList(init_session_msg);
        if (gui_pipe.isValid() && gui_pipe.isConnected()) {
            gui_pipe.writeLine(init_session_str);
            LOG_INFO("Sent INIT_SESSION_LIST to gui");
        }
    } else {
        LOG_WARN("Not all processes connected, skipping INIT_LIST distribution");
    }

    // 全量同步定时器线程
    std::thread sync_timer([&monitor_pipe]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            if (!monitor_pipe.isValid() || !monitor_pipe.isConnected()) {
                break;
            }
            onFullSyncTimer(monitor_pipe);
        }
    });

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

    sync_timer.join();

    monitor_pipe.close();
    executor_pipe.close();
    gui_pipe.close();

    cleanup(managed_processes, job_handle);

    g_plugin_manager.reset();

    LOG_INFO("=== Launcher exited ===");
    return 0;
}