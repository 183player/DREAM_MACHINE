// src/launcher/main.cpp
#include "logger.h"
#include "pipe.h"
#include "process.h"
#include "constants.h"
#include "plugin_manager.h"
#include "messages.h"
#include "message_router.h"
#include "error_codes.h"
#include "error_notify_bridge.h"   // A.11.2a：ErrorContext → ErrorNotifyMessage 映射
#include "event_loop.h"
#include "common_utils.h"

// dm_signal（A.8 装配）
#include "signal_bus.h"
#include "message_bridge.h"
#include "signal_types.h"
#include "signal_strings.h"

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
    void onProcessExit(EventType type, void* user_data);
    void onPipeReadable(EventType type, void* user_data);
    void onHeartbeat(EventType type, void* user_data);
    void requestGracefulShutdown(const std::string& reason);
    void registerMessageHandlers(MessageRouter& router);
    void attachSignalBus();
    void detachSignalBus();
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

// ================================================================
// LauncherState：launcher 全局状态收敛载体（A.3.1 C3）
// ================================================================
struct LauncherState {
    // ---- 通信资源 ----
    NamedPipe* monitor_pipe = nullptr;
    NamedPipe* executor_pipe = nullptr;
    NamedPipe* gui_pipe = nullptr;

    // ---- 进程资源 ----
    std::vector<std::shared_ptr<Process>> managed_processes;

    // ---- 会话状态 ----
    std::unordered_map<std::string, SessionState> sessions;
    std::mutex sessions_mutex;

    // ---- 生命周期 ----
    std::atomic<bool> shutdown_requested{false};
    std::atomic<bool> grace_timer_started{false};
    EventHandle grace_timer_handle{0, false};
    EventLoop* event_loop = nullptr;

    // ---- 插件 ----
    std::unique_ptr<PluginManager> plugin_manager;

    // ---- 消息分发表 ----
    MessageRouter message_router;

    // ---- 诊断 ----
    bool job_info_logged = false;

    // ---- 心跳 ----
    int heartbeat_counter = 0;
};

static LauncherState g_state;

// ================================================================
// publish_error：通过 SignalBus 发布结构化错误（A.11.2a B3 接线）
//
// 单一映射路径（D5 §8.2 / D3 DREAM-010）：
//   ErrorContext 语义 → SignalPayload → SignalBus
//     ├→ Logger（进程内日志，[signal:error_notify] 前缀）
//     └→ MessageBridge → Sender 重建 ErrorNotifyMessage → 跨进程
//
// 说明：
//   - severity 走 error_severity_to_signal_level_int（受控映射，D3 DREAM-010）
//   - error_code 走 errorCodeToString（受控字符串）
//   - extra["error_code"] 携带错误码，供 Sender 重建 details
//   - 不直接 writeLine：让 Logger / MessageBridge 各自消费
//
// 参数：
//   code       - 错误码（ErrorCode）
//   severity   - 严重级别（ErrorSeverity）
//   message    - 人类可读消息
//   details    - 补充详情（可选）
//   session_id - 会话 ID（可选）
// ================================================================
void publish_error(ErrorCode code,
                   ErrorSeverity severity,
                   const std::string& message,
                   const std::string& details = "",
                   const std::string& session_id = "") {
    signal::SignalPayload payload;
    payload.type = signal::SignalType::SGT_ERROR_NOTIFY;
    payload.level = static_cast<signal::SignalLevel>(
        error_severity_to_signal_level_int(severity));
    payload.timestamp_ms = signal::now_ms();
    payload.description = message;
    payload.detail = details;
    if (!session_id.empty()) {
        payload.session_id = session_id;
    }
    // 携带错误码（Sender 重建 details 时使用）
    payload.extra["error_code"] = errorCodeToString(code);

    signal::SignalBus::instance().publish(payload);
}

// ================================================================
// dm_signal 装配（A.8 装配 + A.11.2a Sender 完整实现）
//
// 订阅顺序：Logger → MessageBridge
// 依据：详见文档13 §3.3
//
// Sender 完整实现（A.11.2a）：
//   - 从 SignalPayload 重建 ErrorNotifyMessage
//   - severity 用 signal_level_to_string（受控字符串）
//   - message 用 payload.description
//   - details 用 payload.detail + extra["error_code"]
//   - 序列化 + writeLine
// ================================================================
void attachSignalBus() {
    Logger::attach_to_signal_bus();
    signal::SignalBus::instance().subscribe(&signal::MessageBridge::instance());

    // launcher → gui（错误通知）
    (void)signal::MessageBridge::instance().add_sender(
        "gui",
        [](const signal::SignalPayload& payload) -> bool {
            if (!g_state.gui_pipe || !g_state.gui_pipe->isValid()) {
                return false;
            }

            // A.11.2a：从 SignalPayload 重建 ErrorNotifyMessage
            ErrorNotifyMessage notify;
            notify.source = "launcher";
            notify.severity = signal::signal_level_to_string(payload.level);
            notify.message = payload.description;

            // details：优先 payload.detail，追加 extra["error_code"]
            std::string details = payload.detail;
            auto it_code = payload.extra.find("error_code");
            if (it_code != payload.extra.end() && !it_code->second.empty()) {
                if (!details.empty()) {
                    details += " | ";
                }
                details += std::string("code=") + it_code->second;
            }
            if (!details.empty()) {
                notify.details = details;
            }

            std::string json = serializeErrorNotify(notify);
            PipeResult result = g_state.gui_pipe->writeLine(json);
            if (result == PipeResult::PIPE_OK) {
                LOG_INFO("ErrorNotify sent to gui: " + payload.description);
                return true;
            }
            LOG_WARN("Failed to send ErrorNotify to gui");
            return false;
        },
        [](const signal::SignalPayload& payload) -> bool {
            return payload.type == signal::SignalType::SGT_ERROR_NOTIFY;
        });

    // launcher → monitor（错误通知）
    (void)signal::MessageBridge::instance().add_sender(
        "monitor",
        [](const signal::SignalPayload& payload) -> bool {
            if (!g_state.monitor_pipe || !g_state.monitor_pipe->isValid()) {
                return false;
            }

            // A.11.2a：从 SignalPayload 重建 ErrorNotifyMessage
            ErrorNotifyMessage notify;
            notify.source = "launcher";
            notify.severity = signal::signal_level_to_string(payload.level);
            notify.message = payload.description;

            std::string details = payload.detail;
            auto it_code = payload.extra.find("error_code");
            if (it_code != payload.extra.end() && !it_code->second.empty()) {
                if (!details.empty()) {
                    details += " | ";
                }
                details += std::string("code=") + it_code->second;
            }
            if (!details.empty()) {
                notify.details = details;
            }

            std::string json = serializeErrorNotify(notify);
            PipeResult result = g_state.monitor_pipe->writeLine(json);
            if (result == PipeResult::PIPE_OK) {
                LOG_INFO("ErrorNotify sent to monitor: " + payload.description);
                return true;
            }
            LOG_WARN("Failed to send ErrorNotify to monitor");
            return false;
        },
        [](const signal::SignalPayload& payload) -> bool {
            return payload.type == signal::SignalType::SGT_ERROR_NOTIFY;
        });

    LOG_INFO("dm_signal attached: Logger + MessageBridge (launcher)");
}

void detachSignalBus() {
    signal::MessageBridge::instance().remove_all_senders();
    Logger::detach_from_signal_bus();
    LOG_INFO("dm_signal detached (launcher)");
}

// ================================================================
// 诊断辅助：查询 Job Object 内当前进程数
// ================================================================
int queryJobProcessCount(HANDLE job_handle) {
    if (!job_handle || job_handle == INVALID_HANDLE_VALUE) {
        return -1;
    }

    DWORD bytes_needed = 0;
    if (!QueryInformationJobObject(job_handle, JobObjectBasicProcessIdList,
                                    nullptr, 0, &bytes_needed)) {
        DWORD err = GetLastError();
        if (err != ERROR_BAD_LENGTH && err != ERROR_MORE_DATA) {
            LOG_WARN("QueryInformationJobObject size query failed: " +
                     std::to_string(err));
            return -1;
        }
    }

    if (bytes_needed == 0) {
        return 0;
    }

    std::vector<char> buffer(bytes_needed);
    if (!QueryInformationJobObject(job_handle, JobObjectBasicProcessIdList,
                                    buffer.data(), bytes_needed, &bytes_needed)) {
        DWORD err = GetLastError();
        LOG_WARN("QueryInformationJobObject data query failed: " +
                 std::to_string(err));
        return -1;
    }

    auto* info = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
    return static_cast<int>(info->NumberOfProcessIdsInList);
}

// ================================================================
// 诊断辅助：Job 句柄继承性检查（仅记录一次）
// ================================================================
void logJobHandleInfoOnce(HANDLE job_handle) {
    if (g_state.job_info_logged) {
        return;
    }
    g_state.job_info_logged = true;

    if (!job_handle || job_handle == INVALID_HANDLE_VALUE) {
        LOG_WARN("Job handle is invalid, cannot check inheritable flag");
        return;
    }

    DWORD flags = 0;
    if (GetHandleInformation(job_handle, &flags)) {
        const bool inheritable = (flags & HANDLE_FLAG_INHERIT) != 0;
        LOG_INFO(std::string("Job handle inheritable: ") +
                 (inheritable ? "yes (WARNING: could leak to children)" : "no"));
    } else {
        LOG_WARN("GetHandleInformation on Job handle failed: " +
                 std::to_string(GetLastError()));
    }
}

bool launchSubprocess(const SubprocessInfo& info,
                      std::vector<std::shared_ptr<Process>>& managed_processes,
                      HANDLE job_handle,
                      DWORD parent_pid) {
    logJobHandleInfoOnce(job_handle);

    ProcessStartOptions options;
    options.executable = info.executable;
    options.args = L"--parent-pid " + std::to_wstring(parent_pid);
    options.inherit_handles = true;
    options.job_handle = job_handle;
    options.creation_flags = ProcessCreationFlags::PROC_NO_WINDOW;
    options.timeout_ms = 2000;

    auto proc = std::make_shared<Process>();
    if (!proc->start(options)) {
        LOG_ERROR("Failed to launch " + common::wideToUtf8(info.name));
        return false;
    }

    managed_processes.push_back(proc);
    LOG_INFO("Launched " + common::wideToUtf8(info.name) +
             " (PID: " + std::to_string(proc->getPid()) +
             ", attached to Job Object)");
    return true;
}

// ================================================================
// 强制清理：terminate 所有存活子进程并关闭 Job Object
// ================================================================
void cleanup(const std::vector<std::shared_ptr<Process>>& managed_processes, HANDLE job_handle) {
    for (const auto& proc : managed_processes) {
        if (!proc) {
            continue;
        }

        const bool was_running = proc->isRunning();
        LOG_INFO("Cleanup check: PID=" + std::to_string(proc->getPid()) +
                 " isRunning=" + (was_running ? "true" : "false"));

        if (was_running) {
            LOG_INFO("Terminating process (PID: " + std::to_string(proc->getPid()) + ")...");
            proc->terminate();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } else {
            LOG_INFO("Process (PID: " + std::to_string(proc->getPid()) + ") already exited");
        }
    }

    if (job_handle && job_handle != INVALID_HANDLE_VALUE) {
        const int remaining = queryJobProcessCount(job_handle);
        if (remaining >= 0) {
            LOG_INFO("Job Object contains " + std::to_string(remaining) +
                     " process(es) before close");
        } else {
            LOG_WARN("Failed to query Job Object process count");
        }

        LOG_INFO("Closing Job Object (KILL_ON_JOB_CLOSE will terminate any remaining processes)...");
        CloseHandle(job_handle);
    }

    LOG_INFO("Cleanup complete");
}

// ================================================================
// 优雅关闭流程
// ================================================================
void requestGracefulShutdown(const std::string& reason) {
    if (g_state.shutdown_requested.exchange(true)) {
        return;
    }

    LOG_INFO("Graceful shutdown requested, reason=" + reason);

    ShutdownMessage msg;
    msg.reason = reason;
    msg.initiator = shutdown_initiator::LAUNCHER;
    std::string json = serializeShutdown(msg);

    auto broadcast = [&json](NamedPipe* pipe, const char* name) {
        if (!pipe) {
            LOG_WARN(std::string("Skip SHUTDOWN to ") + name + " (null pipe)");
            return;
        }
        if (!pipe->isValid()) {
            LOG_WARN(std::string("Skip SHUTDOWN to ") + name + " (invalid pipe handle)");
            return;
        }
        if (pipe->isBroken()) {
            LOG_INFO(std::string("Skip SHUTDOWN to ") + name + " (peer already exited)");
            return;
        }
        PipeResult result = pipe->writeLine(json);
        if (result != PipeResult::PIPE_OK) {
            LOG_WARN(std::string("Failed to send SHUTDOWN to ") + name +
                     ", result=" + std::to_string(static_cast<int>(result)));
        } else {
            LOG_INFO(std::string("SHUTDOWN sent to ") + name);
        }
    };

    broadcast(g_state.monitor_pipe, "monitor");
    broadcast(g_state.executor_pipe, "executor");
    broadcast(g_state.gui_pipe, "gui");

    if (!g_state.grace_timer_started.exchange(true)) {
        if (g_state.event_loop) {
            g_state.grace_timer_handle = g_state.event_loop->registerTimer(
                constants::SHUTDOWN_GRACE_MS,
                [](EventType, void*) {
                    LOG_INFO("Grace period expired, forcing shutdown");
                    if (g_state.event_loop) {
                        g_state.event_loop->stop();
                    }
                },
                nullptr,
                true
            );
            if (!g_state.grace_timer_handle.active) {
                LOG_WARN("Failed to register grace timer, falling back to immediate stop");
                if (g_state.event_loop) {
                    g_state.event_loop->stop();
                }
            } else {
                LOG_INFO("Grace timer started (" +
                         std::to_string(constants::SHUTDOWN_GRACE_MS) + "ms)");
            }
        } else {
            LOG_WARN("Event loop not available, immediate shutdown");
        }
    }
}

// ================================================================
// 会话状态处理
// ================================================================
void handleSessionStateChange(const std::string& payload, NamedPipe& gui_pipe) {
    auto msg = parseSessionStateChanged(payload);
    if (!msg.has_value()) {
        LOG_WARN("Failed to parse SESSION_STATE_CHANGED message");
        // A.11.2a：结构化错误信号
        publish_error(ErrorCode::JSON_ERROR,
                      ErrorSeverity::WARNING,
                      "Failed to parse SESSION_STATE_CHANGED",
                      "parseSessionStateChanged returned nullopt");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_state.sessions_mutex);
        auto it = g_state.sessions.find(msg->session_id);
        if (it == g_state.sessions.end()) {
            SessionState new_state;
            new_state.session_id = msg->session_id;
            new_state.state = msg->state;
            new_state.pipe_name = msg->pipe_name.value_or("");
            new_state.last_update = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            g_state.sessions[msg->session_id] = new_state;
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

void handleFullSyncResponse(const std::string& payload) {
    auto resp = parseFullSyncResponse(payload);
    if (!resp.has_value()) {
        LOG_WARN("Failed to parse FULL_SYNC_RESPONSE");
        // A.11.2a：结构化错误信号
        publish_error(ErrorCode::JSON_ERROR,
                      ErrorSeverity::WARNING,
                      "Failed to parse FULL_SYNC_RESPONSE",
                      "parseFullSyncResponse returned nullopt");
        return;
    }

    LOG_INFO("Full sync response received (request_id: " + std::to_string(resp->request_id) +
             ", sessions: " + std::to_string(resp->sessions.size()) + ")");

    {
        std::lock_guard<std::mutex> lock(g_state.sessions_mutex);
        g_state.sessions.clear();
        for (const auto& s : resp->sessions) {
            SessionState state;
            state.session_id = s.session_id;
            state.state = s.state;
            state.pipe_name = s.pipe_name.value_or("");
            state.last_update = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            g_state.sessions[s.session_id] = state;
            LOG_INFO("Sync: session " + s.session_id + " -> " + s.state);
        }
    }
}

// ================================================================
// 消息 handler 注册
// ================================================================
void registerMessageHandlers(MessageRouter& router) {
    // ---- PLUGIN_IMPORT ----
    router.register_handler(msg_types::PLUGIN_IMPORT,
        [](const std::string& payload, void* ctx) {
            auto* pipe = static_cast<NamedPipe*>(ctx);
            if (!pipe) return;

            auto import_msg = parsePluginImport(payload);
            if (!import_msg.has_value()) return;

            std::string plugin_id;
            bool success = g_state.plugin_manager->importPlugin(import_msg->package_path, plugin_id);
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
        });

    // ---- PLUGIN_DELETE ----
    router.register_handler(msg_types::PLUGIN_DELETE,
        [](const std::string& payload, void* ctx) {
            auto* pipe = static_cast<NamedPipe*>(ctx);
            if (!pipe) return;

            auto delete_msg = parsePluginDelete(payload);
            if (!delete_msg.has_value()) return;

            bool success = g_state.plugin_manager->deletePlugin(delete_msg->plugin_id);
            PluginDeleteRespMessage resp;
            resp.success = success;
            if (!success) {
                resp.error = "Delete failed";
            }
            std::string resp_json = serializePluginDeleteResp(resp);
            pipe->writeLine(resp_json);
        });

    // ---- PLUGIN_ENABLE ----
    router.register_handler(msg_types::PLUGIN_ENABLE,
        [](const std::string& payload, void* ctx) {
            auto* pipe = static_cast<NamedPipe*>(ctx);
            if (!pipe) return;

            auto enable_msg = parsePluginEnable(payload);
            if (!enable_msg.has_value()) return;

            bool success = g_state.plugin_manager->setPluginEnabled(enable_msg->plugin_id,
                                                                    enable_msg->enabled);
            PluginEnableRespMessage resp;
            resp.success = success;
            if (!success) {
                resp.error = "Enable/disable failed";
            }
            std::string resp_json = serializePluginEnableResp(resp);
            pipe->writeLine(resp_json);
        });

    // ---- FULL_SYNC_RESPONSE ----
    router.register_handler(msg_types::FULL_SYNC_RESPONSE,
        [](const std::string& payload, void* /*ctx*/) {
            handleFullSyncResponse(payload);
        });

    // ---- SESSION_STATE_CHANGED ----
    router.register_handler(msg_types::SESSION_STATE_CHANGED,
        [](const std::string& payload, void* ctx) {
            auto* pipe = static_cast<NamedPipe*>(ctx);
            if (pipe == g_state.gui_pipe) {
                LOG_WARN("SESSION_STATE_CHANGED should come from monitor, not gui");
            } else {
                if (g_state.gui_pipe) {
                    handleSessionStateChange(payload, *g_state.gui_pipe);
                }
            }
        });
}

// ================================================================
// 事件回调函数
// ================================================================

void onProcessExit(EventType type, void* user_data) {
    (void)type;
    Process* proc = static_cast<Process*>(user_data);
    if (!proc) {
        return;
    }

    LOG_INFO("Subprocess (PID: " + std::to_string(proc->getPid()) + ") has exited");

    {
        std::string snapshot = "Process snapshot:";
        bool all_exited = true;
        for (const auto& p : g_state.managed_processes) {
            if (!p) {
                continue;
            }
            const bool running = p->isRunning();
            snapshot += " PID=" + std::to_string(p->getPid()) +
                        "=" + (running ? "R" : "X");
            if (running) {
                all_exited = false;
            }
        }
        snapshot += ", all_exited=" + std::string(all_exited ? "true" : "false");
        LOG_INFO(snapshot);

        requestGracefulShutdown(shutdown_reason::PEER_EXIT);

        if (all_exited) {
            LOG_INFO("All subprocesses exited, stopping event loop immediately");
            if (g_state.event_loop) {
                g_state.event_loop->stop();
            }
        }
    }
}

// ================================================================
// onPipeReadable
//
// A.4  C1：移除 ACTIVE_SESSIONS_RESP fallback，统一走 router。
// A.11.2a B3：pipe broken 时发布结构化错误信号。
// ================================================================
void onPipeReadable(EventType type, void* user_data) {
    (void)type;
    if (!user_data || g_state.shutdown_requested) return;

    NamedPipe* pipe = static_cast<NamedPipe*>(user_data);

    // A.11.2a：管道无效/断开时结构化错误（替代静默 WARN）
    if (!pipe->isValid() || pipe->isBroken()) {
        LOG_WARN("Pipe invalid or broken");
        publish_error(ErrorCode::PIPE_BROKEN,
                      ErrorSeverity::RECOVERABLE,
                      "Launcher pipe invalid or broken",
                      "isBroken() returned true");
        requestGracefulShutdown(shutdown_reason::PEER_EXIT);
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
            if (!g_state.message_router.dispatch(type_str, payload, pipe)) {
                LOG_WARN("Unhandled message type: " + type_str);
            }
        } else {
            LOG_WARN("Failed to parse base message");
            publish_error(ErrorCode::JSON_ERROR,
                          ErrorSeverity::WARNING,
                          "Failed to parse base message",
                          "parseBaseMessage returned false");
        }
    } else if (read_result == PipeResult::PIPE_BROKEN) {
        LOG_WARN("Pipe broken during read");
        publish_error(ErrorCode::PIPE_BROKEN,
                      ErrorSeverity::RECOVERABLE,
                      "Launcher pipe broken during read",
                      "readLineBuffered returned PIPE_BROKEN");
        requestGracefulShutdown(shutdown_reason::PEER_EXIT);
    }
}

void onHeartbeat(EventType type, void* user_data) {
    (void)type;
    (void)user_data;

    ++g_state.heartbeat_counter;
    if (g_state.heartbeat_counter % 100 == 0) {
        std::lock_guard<std::mutex> lock(g_state.sessions_mutex);
        LOG_INFO("Launcher heartbeat: " + std::to_string(g_state.heartbeat_counter) +
                 " iterations, sessions: " + std::to_string(g_state.sessions.size()));
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
//
// 本分片合并改动：
//   A.3.1    C3 全局状态收敛（LauncherState g_state）
//   A.4      C1 分发表下沉（移除 ACTIVE_SESSIONS_RESP fallback）
//   A.8      dm_signal 装配（Logger attach + MessageBridge subscribe + sender）
//   A.11.2a  B3 错误结构化接线（Sender 完整实现 + isBroken 结构化错误）
// ================================================================
int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "--show-plugin-info") {
        return showPluginInfo();
    }

    Logger::instance().setProcessName("launcher");

    Logger::instance().setLogDirectory(common::pathFromRoot("logs"));

    const bool archived_prev = Logger::instance().archiveLastSessionIfDirty();

    LOG_INFO("=== Dream Machine Launcher starting ===");

    if (archived_prev) {
        LOG_INFO("Previous session logs archived to logs/crashes/");
    }

    // A.8：dm_signal 装配（Logger + MessageBridge）
    attachSignalBus();

    registerMessageHandlers(g_state.message_router);

    g_state.plugin_manager = std::make_unique<PluginManager>();

    if (!g_state.plugin_manager->verifySystemPlugins()) {
        LOG_WARN("System plugin integrity check failed, continuing anyway");
        // A.11.2a：结构化错误信号
        publish_error(ErrorCode::PLUGIN_VERIFY_FAILED,
                      ErrorSeverity::WARNING,
                      "System plugin integrity check failed",
                      "verifySystemPlugins returned false; continuing");
    }

    if (!g_state.plugin_manager->scanPlugins()) {
        LOG_WARN("Plugin scan failed, continuing without plugins");
        publish_error(ErrorCode::PLUGIN_LOAD_FAILED,
                      ErrorSeverity::WARNING,
                      "Plugin scan failed",
                      "scanPlugins returned false; continuing without plugins");
    }

    plugin::InitList init_list = g_state.plugin_manager->generateInitList();

    HANDLE job_handle = CreateJobObjectW(nullptr, L"Global\\DreamMachine_Launcher_Job");
    if (!job_handle) {
        LOG_ERROR("Failed to create Job Object: error " + std::to_string(GetLastError()));
        publish_error(ErrorCode::PROCESS_LAUNCH_FAILED,
                      ErrorSeverity::FATAL,
                      "Failed to create Job Object",
                      "CreateJobObjectW returned null");
    } else {
        LOG_INFO("Job Object created successfully");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
        job_info.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
            JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
        if (!SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation,
                                     &job_info, sizeof(job_info))) {
            LOG_ERROR("Failed to configure Job Object: error " + std::to_string(GetLastError()));
            publish_error(ErrorCode::PROCESS_LAUNCH_FAILED,
                          ErrorSeverity::FATAL,
                          "Failed to configure Job Object",
                          "SetInformationJobObject failed");
        } else {
            LOG_INFO("Job Object configured: KILL_ON_JOB_CLOSE + SILENT_BREAKAWAY_OK enabled");
        }

        LOG_INFO("Job Object name: Global\\DreamMachine_Launcher_Job, handle=" +
                 std::to_string(reinterpret_cast<uintptr_t>(job_handle)));
    }

    constexpr int MAX_INSTANCES = 1;

    std::string monitor_pipe_name_str = pipe_names::launcher_monitor();
    std::string executor_pipe_name_str = pipe_names::launcher_executor();
    std::string gui_pipe_name_str = pipe_names::launcher_gui();

    std::wstring monitor_pipe_name = common::utf8ToWide(monitor_pipe_name_str);
    std::wstring executor_pipe_name = common::utf8ToWide(executor_pipe_name_str);
    std::wstring gui_pipe_name = common::utf8ToWide(gui_pipe_name_str);

    NamedPipe monitor_pipe;
    NamedPipe executor_pipe;
    NamedPipe gui_pipe;

    LOG_INFO("Creating three pipe instances for monitor, executor, gui...");

    if (!monitor_pipe.createServer(monitor_pipe_name, MAX_INSTANCES, true)) {
        LOG_ERROR("Failed to create monitor pipe server");
        publish_error(ErrorCode::PIPE_CREATE_FAILED,
                      ErrorSeverity::FATAL,
                      "Failed to create monitor pipe server",
                      monitor_pipe_name_str);
        cleanup(g_state.managed_processes, job_handle);
        detachSignalBus();
        return 1;
    }

    if (!executor_pipe.createServer(executor_pipe_name, MAX_INSTANCES, true)) {
        LOG_ERROR("Failed to create executor pipe server");
        publish_error(ErrorCode::PIPE_CREATE_FAILED,
                      ErrorSeverity::FATAL,
                      "Failed to create executor pipe server",
                      executor_pipe_name_str);
        cleanup(g_state.managed_processes, job_handle);
        detachSignalBus();
        return 1;
    }

    if (!gui_pipe.createServer(gui_pipe_name, MAX_INSTANCES, true)) {
        LOG_ERROR("Failed to create gui pipe server");
        publish_error(ErrorCode::PIPE_CREATE_FAILED,
                      ErrorSeverity::FATAL,
                      "Failed to create gui pipe server",
                      gui_pipe_name_str);
        cleanup(g_state.managed_processes, job_handle);
        detachSignalBus();
        return 1;
    }

    LOG_INFO("All three pipe servers created successfully (secure mode enabled)");

    std::vector<SubprocessInfo> subprocesses;
    subprocesses.push_back({L"monitor", L"monitor.exe"});
    subprocesses.push_back({L"executor", L"executor.exe"});
    subprocesses.push_back({L"gui", L"gui.exe"});

    DWORD parent_pid = GetCurrentProcessId();
    for (const auto& info : subprocesses) {
        if (!launchSubprocess(info, g_state.managed_processes, job_handle, parent_pid)) {
            LOG_ERROR("Failed to launch " + common::wideToUtf8(info.name));
            publish_error(ErrorCode::PROCESS_LAUNCH_FAILED,
                          ErrorSeverity::FATAL,
                          "Failed to launch subprocess",
                          common::wideToUtf8(info.name));
        }
    }

    if (g_state.managed_processes.size() != 3) {
        LOG_WARN("Only " + std::to_string(g_state.managed_processes.size()) +
                 "/3 subprocesses started successfully");
        publish_error(ErrorCode::PROCESS_LAUNCH_FAILED,
                      ErrorSeverity::WARNING,
                      "Not all subprocesses started",
                      std::to_string(g_state.managed_processes.size()) + "/3");
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
        publish_error(ErrorCode::PIPE_CONNECT_FAILED,
                      ErrorSeverity::WARNING,
                      "Not all subprocesses connected",
                      std::to_string(connected_count) + "/3");
    } else {
        LOG_INFO("All subprocesses connected successfully");
    }

    if (connected_count == 3) {
        bool dist_ok = g_state.plugin_manager->distributeInitList(
            gui_pipe, executor_pipe, monitor_pipe, init_list);
        if (dist_ok) {
            LOG_INFO("INIT_LIST distributed to all processes");
        } else {
            LOG_WARN("INIT_LIST distribution incomplete");
            publish_error(ErrorCode::PLUGIN_LOAD_FAILED,
                          ErrorSeverity::WARNING,
                          "INIT_LIST distribution incomplete",
                          "distributeInitList returned false");
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

    g_state.monitor_pipe = &monitor_pipe;
    g_state.executor_pipe = &executor_pipe;
    g_state.gui_pipe = &gui_pipe;

    EventLoop event_loop;
    g_state.event_loop = &event_loop;

    for (auto& proc : g_state.managed_processes) {
        EventHandle handle = event_loop.registerWaitable(proc->getHandle(), onProcessExit, proc.get());
        if (!handle.active) {
            LOG_ERROR("Failed to register waitable for process PID: " + std::to_string(proc->getPid()));
        } else {
            LOG_INFO("Registered process waitable for PID: " + std::to_string(proc->getPid()));
        }
    }

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

    EventHandle heartbeat_handle = event_loop.registerTimer(100, onHeartbeat, nullptr, false);
    if (!heartbeat_handle.active) {
        LOG_WARN("Failed to register heartbeat timer");
    }

    EventHandle stop_signal = event_loop.registerSignal([](EventType type, void* data) {
        (void)type;
        (void)data;
        LOG_INFO("Stop signal received");
        requestGracefulShutdown(shutdown_reason::SIGNAL);
    });
    if (!stop_signal.active) {
        LOG_WARN("Failed to register stop signal");
    }

    LOG_INFO("Entering event-driven main loop...");
    event_loop.run();

    LOG_INFO("Shutting down launcher...");

    event_loop.unregister(monitor_handle);
    event_loop.unregister(executor_handle);
    event_loop.unregister(gui_handle);
    event_loop.unregister(heartbeat_handle);
    event_loop.unregister(stop_signal);

    monitor_pipe.close();
    executor_pipe.close();
    gui_pipe.close();

    cleanup(g_state.managed_processes, job_handle);

    // A.8：dm_signal 卸载（与 attachSignalBus 配对）
    detachSignalBus();

    g_state.plugin_manager.reset();
    g_state.event_loop = nullptr;

    LOG_INFO("=== Launcher exited ===");

    Logger::instance().markCleanExit();

    return 0;
}