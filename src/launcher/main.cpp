// src/launcher/main.cpp
#include "logger.h"
#include "pipe.h"
#include "process.h"
#include "constants.h"
#include "plugin_manager.h"
#include "messages.h"
#include "error_codes.h"
#include "event_loop.h"

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <iomanip>
#include <atomic>
#include <memory>

using namespace dream_machine;
using namespace dream_machine::launcher;
using namespace dream_machine::event;

// ================================================================
// 前向声明
// ================================================================
namespace {
    void handleFullSyncResponse(const std::string& payload);
    void handleSessionStateChange(const std::string& payload, NamedPipe& gui_pipe);
    void handleActiveSessionsResp(const std::string& message, NamedPipe& gui_pipe);
    void onProcessExit(EventType type, void* user_data);
    void onPipeReadable(EventType type, void* user_data);
    void onHeartbeat(EventType type, void* user_data);
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

// 全局状态
std::unordered_map<std::string, SessionState> sessions_;
std::mutex sessions_mutex_;
std::unique_ptr<PluginManager> g_plugin_manager;
std::atomic<bool> g_shutdown_requested{false};

// 子进程管理
std::vector<Process> g_managed_processes;
NamedPipe* g_monitor_pipe = nullptr;
NamedPipe* g_executor_pipe = nullptr;
NamedPipe* g_gui_pipe = nullptr;

// 事件循环指针（用于回调中停止）
EventLoop* g_event_loop = nullptr;

// 用于在回调中访问的上下文
struct LauncherContext {
    NamedPipe* monitor_pipe;
    NamedPipe* executor_pipe;
    NamedPipe* gui_pipe;
    std::vector<Process>* processes;
    std::atomic<bool>* shutdown;
};
std::unique_ptr<LauncherContext> g_context;

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

void handleFullSyncResponse(const std::string& payload) {
    auto resp = parseFullSyncResponse(payload);
    if (!resp.has_value()) {
        LOG_WARN("Failed to parse FULL_SYNC_RESPONSE");
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
}

// ================================================================
// 事件回调函数
// ================================================================

// 子进程退出回调
void onProcessExit(EventType type, void* user_data) {
    (void)type;
    Process* proc = static_cast<Process*>(user_data);
    LOG_INFO("Subprocess (PID: " + std::to_string(proc->getPid()) + ") has exited");
    g_shutdown_requested = true;
    if (g_event_loop) {
        g_event_loop->stop();
        LOG_INFO("Event loop stop requested from process exit callback");
    }
}

// 管道可读回调（处理消息）
void onPipeReadable(EventType type, void* user_data) {
    (void)type;
    if (!user_data || g_shutdown_requested) return;

    NamedPipe* pipe = static_cast<NamedPipe*>(user_data);
    if (!pipe->isValid() || pipe->isBroken()) {
        LOG_WARN("Pipe invalid or broken");
        g_shutdown_requested = true;
        if (g_event_loop) {
            g_event_loop->stop();
            LOG_INFO("Event loop stop requested from pipe broken callback");
        }
        return;
    }

    DWORD bytes_available = 0;
    PipeResult peek_result = pipe->peekAvailable(bytes_available);
    if (peek_result != PipeResult::PIPE_OK || bytes_available == 0) {
        return;
    }

    std::string message;
    PipeResult read_result = pipe->readLineBuffered(message, 3000);
    if (read_result == PipeResult::PIPE_OK) {
        LOG_INFO("Received: " + message);

        std::string type_str, cmd, payload;
        if (parseBaseMessage(message, type_str, cmd, payload)) {
            if (type_str == msg_types::PLUGIN_IMPORT) {
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
                    pipe->writeLine(resp_json);
                }
            }
            else if (type_str == msg_types::PLUGIN_DELETE) {
                auto delete_msg = parsePluginDelete(payload);
                if (delete_msg.has_value()) {
                    bool success = g_plugin_manager->deletePlugin(delete_msg->plugin_id);
                    PluginDeleteRespMessage resp;
                    resp.success = success;
                    if (!success) {
                        resp.error = "Delete failed";
                    }
                    std::string resp_json = serializePluginDeleteResp(resp);
                    pipe->writeLine(resp_json);
                }
            }
            else if (type_str == msg_types::PLUGIN_ENABLE) {
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
                    pipe->writeLine(resp_json);
                }
            }
            else if (type_str == msg_types::FULL_SYNC_RESPONSE) {
                handleFullSyncResponse(payload);
            }
            else if (type_str == msg_types::SESSION_STATE_CHANGED) {
                if (pipe == g_gui_pipe) {
                    LOG_WARN("SESSION_STATE_CHANGED should come from monitor, not gui");
                } else {
                    handleSessionStateChange(payload, *g_gui_pipe);
                }
            }
            else if (type_str == msg_types::ACTIVE_SESSIONS_RESP) {
                handleActiveSessionsResp(message, *g_gui_pipe);
            }
        } else {
            LOG_WARN("Failed to parse base message");
        }
    } else if (read_result == PipeResult::PIPE_BROKEN) {
        LOG_WARN("Pipe broken during read");
        g_shutdown_requested = true;
        if (g_event_loop) {
            g_event_loop->stop();
            LOG_INFO("Event loop stop requested from read broken callback");
        }
    }
}

// 心跳日志（定时回调）
int g_heartbeat_counter = 0;
void onHeartbeat(EventType type, void* user_data) {
    (void)type;
    (void)user_data;

    ++g_heartbeat_counter;
    if (g_heartbeat_counter % 100 == 0) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        LOG_INFO("Launcher heartbeat: " + std::to_string(g_heartbeat_counter) +
                 " iterations, sessions: " + std::to_string(sessions_.size()));
    }
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
        cleanup(g_managed_processes, job_handle);
        return 1;
    }

    if (!executor_pipe.createServer(executor_pipe_name, MAX_INSTANCES, true)) {
        LOG_ERROR("Failed to create executor pipe server");
        cleanup(g_managed_processes, job_handle);
        return 1;
    }

    if (!gui_pipe.createServer(gui_pipe_name, MAX_INSTANCES, true)) {
        LOG_ERROR("Failed to create gui pipe server");
        cleanup(g_managed_processes, job_handle);
        return 1;
    }

    LOG_INFO("All three pipe servers created successfully (secure mode enabled)");

    std::vector<SubprocessInfo> subprocesses;
    subprocesses.push_back({L"monitor", L"monitor.exe"});
    subprocesses.push_back({L"executor", L"executor.exe"});
    subprocesses.push_back({L"gui", L"gui.exe"});

    DWORD parent_pid = GetCurrentProcessId();
    for (const auto& info : subprocesses) {
        if (!launchSubprocess(info, g_managed_processes, job_handle, parent_pid)) {
            LOG_ERROR("Failed to launch " + std::string(info.name.begin(), info.name.end()));
        }
    }

    if (g_managed_processes.size() != 3) {
        LOG_WARN("Only " + std::to_string(g_managed_processes.size()) +
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

    // ----- 保存指针供回调使用 -----
    g_monitor_pipe = &monitor_pipe;
    g_executor_pipe = &executor_pipe;
    g_gui_pipe = &gui_pipe;

    // ----- 创建事件循环 -----
    EventLoop event_loop;
    g_event_loop = &event_loop;   // 保存全局指针以便回调中使用

    // 注册子进程退出事件
    for (auto& proc : g_managed_processes) {
        EventHandle handle = event_loop.registerWaitable(proc.getHandle(), onProcessExit, &proc);
        if (!handle.active) {
            LOG_ERROR("Failed to register waitable for process PID: " + std::to_string(proc.getPid()));
        } else {
            LOG_INFO("Registered process waitable for PID: " + std::to_string(proc.getPid()));
        }
    }

    // 注册管道可读事件
    EventHandle monitor_handle = event_loop.registerReadable(monitor_pipe.getHandle(), onPipeReadable, &monitor_pipe);
    if (!monitor_handle.active) {
        LOG_ERROR("Failed to register readable for monitor pipe");
    } else {
        LOG_INFO("Registered readable for monitor pipe");
    }

    EventHandle executor_handle = event_loop.registerReadable(executor_pipe.getHandle(), onPipeReadable, &executor_pipe);
    if (!executor_handle.active) {
        LOG_ERROR("Failed to register readable for executor pipe");
    } else {
        LOG_INFO("Registered readable for executor pipe");
    }

    EventHandle gui_handle = event_loop.registerReadable(gui_pipe.getHandle(), onPipeReadable, &gui_pipe);
    if (!gui_handle.active) {
        LOG_ERROR("Failed to register readable for gui pipe");
    } else {
        LOG_INFO("Registered readable for gui pipe");
    }

    // 注册心跳定时器（每 100ms 计数，每 100 次输出日志）
    EventHandle heartbeat_handle = event_loop.registerTimer(100, onHeartbeat, nullptr, false);
    if (!heartbeat_handle.active) {
        LOG_WARN("Failed to register heartbeat timer");
    }

    // 注册停止信号（用于外部停止）
    EventHandle stop_signal = event_loop.registerSignal([](EventType type, void* data) {
        (void)type;
        (void)data;
        LOG_INFO("Stop signal received");
        g_shutdown_requested = true;
        if (g_event_loop) {
            g_event_loop->stop();
        }
    });
    if (!stop_signal.active) {
        LOG_WARN("Failed to register stop signal");
    }

    LOG_INFO("Entering event-driven main loop...");
    event_loop.run();

    // ----- 清理 -----
    LOG_INFO("Shutting down launcher...");

    // 取消注册所有事件
    event_loop.unregister(monitor_handle);
    event_loop.unregister(executor_handle);
    event_loop.unregister(gui_handle);
    event_loop.unregister(heartbeat_handle);
    event_loop.unregister(stop_signal);

    // 关闭管道
    monitor_pipe.close();
    executor_pipe.close();
    gui_pipe.close();

    // 清理子进程
    cleanup(g_managed_processes, job_handle);

    g_plugin_manager.reset();
    g_event_loop = nullptr;

    LOG_INFO("=== Launcher exited ===");
    return 0;
}