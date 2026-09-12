// src/launcher/main.cpp
#include "logger.h"
#include "pipe.h"
#include "process.h"
#include "constants.h"
#include "plugin_manager.h"
#include "messages.h"
#include "message_router.h"
#include "error_codes.h"
#include "event_loop.h"
#include "common_utils.h"

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
// 注：阶段 1.7 C4 起不再 using dream_machine::common——
//     新增的 utf8ToWide / wideToUtf8 用 common:: 前缀显式调用。

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
    void requestGracefulShutdown(const std::string& reason);
    void registerMessageHandlers(MessageRouter& router);
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

// 子进程管理（shared_ptr 管理生命周期，为未来业务内聚线程预留）
std::vector<std::shared_ptr<Process>> g_managed_processes;
NamedPipe* g_monitor_pipe = nullptr;
NamedPipe* g_executor_pipe = nullptr;
NamedPipe* g_gui_pipe = nullptr;

// 事件循环指针（用于回调中停止）
EventLoop* g_event_loop = nullptr;

// 优雅关闭定时器（500ms 自愿窗口）
std::atomic<bool> g_grace_timer_started{false};
EventHandle g_grace_timer_handle{0, false};

// 诊断：Job 句柄信息仅记录一次
bool g_job_info_logged = false;

// 消息分发表（阶段 1.7 C1）
//
// 迁移了 5 个 handler（PLUGIN_IMPORT / PLUGIN_DELETE / PLUGIN_ENABLE /
// FULL_SYNC_RESPONSE / SESSION_STATE_CHANGED）。
// ACTIVE_SESSIONS_RESP 未迁移——它需要完整 message 字符串（转发给 gui），
// handler 签名只接收 payload，故保留在原 if-else fallback 中。
//
// 回退方式：删除本变量 + registerMessageHandlers 调用，
//          并将 onPipeReadable 恢复为原始 if-else 即可。
MessageRouter g_message_router;

// 用于在回调中访问的上下文
// 注：当前未被实际使用（保留为未来状态收敛的载体，见 F1.5-C3）
struct LauncherContext {
    NamedPipe* monitor_pipe;
    NamedPipe* executor_pipe;
    NamedPipe* gui_pipe;
    std::vector<std::shared_ptr<Process>>* processes;
    std::atomic<bool>* shutdown;
};
std::unique_ptr<LauncherContext> g_context;

// ================================================================
// 诊断辅助：查询 Job Object 内当前进程数
//
// 用于 cleanup() 关闭 Job 前确认残留情况。
// 失败时返回 -1（不影响主流程）。
//
// 依据：阶段 1.6 诊断增强 D1
//
// 错误码说明（阶段 1.6 修复）：
//   QueryInformationJobObject 在 buffer=NULL 时返回
//   ERROR_BAD_LENGTH(24)，而非 ERROR_MORE_DATA(234)。
//   实测日志出现 "size query failed: 24" 即此原因。
//   修复：同时接受 ERROR_BAD_LENGTH 与 ERROR_MORE_DATA
//        （旧版 Windows 可能返回后者）。
// ================================================================
int queryJobProcessCount(HANDLE job_handle) {
    if (!job_handle || job_handle == INVALID_HANDLE_VALUE) {
        return -1;
    }

    // 第一次调用：查询所需 buffer 大小
    DWORD bytes_needed = 0;
    if (!QueryInformationJobObject(job_handle, JobObjectBasicProcessIdList,
                                    nullptr, 0, &bytes_needed)) {
        DWORD err = GetLastError();
        // ERROR_BAD_LENGTH：buffer 为 NULL 时的正常返回值（需更多空间）
        // ERROR_MORE_DATA：buffer 不足时的返回值（旧 Windows）
        if (err != ERROR_BAD_LENGTH && err != ERROR_MORE_DATA) {
            LOG_WARN("QueryInformationJobObject size query failed: " +
                     std::to_string(err));
            return -1;
        }
    }

    // bytes_needed == 0 表示 Job 中无进程
    if (bytes_needed == 0) {
        return 0;
    }

    // 第二次调用：获取实际数据
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
//
// 依据：阶段 1.6 诊断增强 D2
// ================================================================
void logJobHandleInfoOnce(HANDLE job_handle) {
    if (g_job_info_logged) {
        return;
    }
    g_job_info_logged = true;

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
    // 诊断：首次启动子进程时记录 Job 句柄信息
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
//
// 依据阶段 1.6 诊断增强 D4：
//   - 每个进程 terminate 前记录 isRunning 状态
//   - Job 关闭前记录剩余进程数
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
//
// 依据 DREAM_MACHINE_SHUTDOWN_COORDINATION 裁决：
//   - 向所有存活直接子进程（monitor / executor / gui）广播 SHUTDOWN
//   - 启动 500ms 一次性定时器（幂等）
//   - 若所有子进程提前退出，由 onProcessExit 提前 stop
//
// 幂等性：g_shutdown_requested.exchange(true) 保证首次进入才执行广播
// 线程安全：本函数可在 onProcessExit / stop_signal 回调中调用
//           （当前单线程事件循环；未来引入业务内聚线程时需重审）
//
// 未来重审点（P3-1 / 依据 ISSUE-LAUNCHER-LAST-EXIT-LOG-MISSING）：
//   最后一个退出进程的 WAITABLE 回调可能未触发（日志少一条）。
//   修复方案：EventLoop 新增 drainPendingEvents()，
//   stop 前处理完所有已 signaled 的 WAITABLE 事件。
//   归入阶段 5（横切打磨），与 C1/C2/C4 同批。
// ================================================================
void requestGracefulShutdown(const std::string& reason) {
    if (g_shutdown_requested.exchange(true)) {
        return;
    }

    LOG_INFO("Graceful shutdown requested, reason=" + reason);

    // ----- 1. 广播 SHUTDOWN 给所有存活直接子进程 -----
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

    broadcast(g_monitor_pipe, "monitor");
    broadcast(g_executor_pipe, "executor");
    broadcast(g_gui_pipe, "gui");

    // ----- 2. 启动 500ms 一次性定时器（幂等） -----
    if (!g_grace_timer_started.exchange(true)) {
        if (g_event_loop) {
            g_grace_timer_handle = g_event_loop->registerTimer(
                constants::SHUTDOWN_GRACE_MS,
                [](EventType, void*) {
                    LOG_INFO("Grace period expired, forcing shutdown");
                    if (g_event_loop) {
                        g_event_loop->stop();
                    }
                },
                nullptr,
                true
            );
            if (!g_grace_timer_handle.active) {
                LOG_WARN("Failed to register grace timer, falling back to immediate stop");
                if (g_event_loop) {
                    g_event_loop->stop();
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
// 消息 handler 注册（阶段 1.7 C1）
//
// 注册 5 个 handler 到全局 router。handler 通过 context (void*)
// 接收消息来源的 NamedPipe*。
//
// 未迁移：ACTIVE_SESSIONS_RESP（需完整 message 字符串，见 router 注释）。
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
        });

    // ---- PLUGIN_DELETE ----
    router.register_handler(msg_types::PLUGIN_DELETE,
        [](const std::string& payload, void* ctx) {
            auto* pipe = static_cast<NamedPipe*>(ctx);
            if (!pipe) return;

            auto delete_msg = parsePluginDelete(payload);
            if (!delete_msg.has_value()) return;

            bool success = g_plugin_manager->deletePlugin(delete_msg->plugin_id);
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

            bool success = g_plugin_manager->setPluginEnabled(enable_msg->plugin_id,
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
    // 消息来源应为 monitor；若来自 gui 则为异常。
    router.register_handler(msg_types::SESSION_STATE_CHANGED,
        [](const std::string& payload, void* ctx) {
            auto* pipe = static_cast<NamedPipe*>(ctx);
            if (pipe == g_gui_pipe) {
                LOG_WARN("SESSION_STATE_CHANGED should come from monitor, not gui");
            } else {
                if (g_gui_pipe) {
                    handleSessionStateChange(payload, *g_gui_pipe);
                }
            }
        });
}

// ================================================================
// 事件回调函数
// ================================================================

// 子进程退出回调
//
// 依据阶段 1.6 诊断增强 D3：
//   记录 all_exited 判断时的完整进程状态快照，
//   便于定位"提前 stop 但最后进程日志缺失"的场景。
void onProcessExit(EventType type, void* user_data) {
    (void)type;
    Process* proc = static_cast<Process*>(user_data);
    if (!proc) {
        return;
    }

    LOG_INFO("Subprocess (PID: " + std::to_string(proc->getPid()) + ") has exited");

    // 诊断 D3：记录所有子进程状态快照
    {
        std::string snapshot = "Process snapshot:";
        bool all_exited = true;
        for (const auto& p : g_managed_processes) {
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

        // 触发优雅关闭（幂等）
        requestGracefulShutdown(shutdown_reason::PEER_EXIT);

        if (all_exited) {
            LOG_INFO("All subprocesses exited, stopping event loop immediately");
            if (g_event_loop) {
                g_event_loop->stop();
            }
        }
    }
}

// 管道可读回调（处理消息）
//
// 阶段 1.7 C1：优先走 MessageRouter，未注册的走 fallback（ACTIVE_SESSIONS_RESP）。
void onPipeReadable(EventType type, void* user_data) {
    (void)type;
    if (!user_data || g_shutdown_requested) return;

    NamedPipe* pipe = static_cast<NamedPipe*>(user_data);
    if (!pipe->isValid() || pipe->isBroken()) {
        LOG_WARN("Pipe invalid or broken");
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
            // ---- 优先走分发表（阶段 1.7 C1） ----
            if (g_message_router.dispatch(type_str, payload, pipe)) {
                return;
            }

            // ---- fallback：未迁移的消息类型 ----
            // 当前仅 ACTIVE_SESSIONS_RESP（需完整 message 转发给 gui）
            if (type_str == msg_types::ACTIVE_SESSIONS_RESP) {
                if (g_gui_pipe) {
                    handleActiveSessionsResp(message, *g_gui_pipe);
                }
            } else {
                LOG_WARN("Unhandled message type: " + type_str);
            }
        } else {
            LOG_WARN("Failed to parse base message");
        }
    } else if (read_result == PipeResult::PIPE_BROKEN) {
        LOG_WARN("Pipe broken during read");
        requestGracefulShutdown(shutdown_reason::PEER_EXIT);
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
//
// 日志生命周期（阶段 1.5 P1-5）：
//   - 启动：setProcessName → archiveLastSessionIfDirty → 开始日志
//   - 退出：最后一条日志 → markCleanExit → return 0
//
// --show-plugin-info 分支与启动失败路径不写 .clean_exit 标记：
// 它们不是正常会话，不应影响下次启动的归档判断。
//
// 阶段 1.6 诊断增强：D1（Job 进程数查询）、D2（Job 句柄继承性）、
//                    D3（进程快照）、D4（cleanup 详细日志）、
//                    D5（Job 句柄信息）—— 仅日志，无行为改动。
//
// 阶段 1.7 C1：消息分发表接入（5 个 handler 迁移）
// ================================================================
int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "--show-plugin-info") {
        return showPluginInfo();
    }

    Logger::instance().setProcessName("launcher");

    const bool archived_prev = Logger::instance().archiveLastSessionIfDirty();

    LOG_INFO("=== Dream Machine Launcher starting ===");

    if (archived_prev) {
        LOG_INFO("Previous session logs archived to logs/crashes/");
    }

    // 阶段 1.7 C1：注册消息 handler（必须在事件循环启动前）
    registerMessageHandlers(g_message_router);

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

        LOG_INFO("Job Object name: Global\\DreamMachine_Launcher_Job, handle=" +
                 std::to_string(reinterpret_cast<uintptr_t>(job_handle)));
    }

    constexpr int MAX_INSTANCES = 1;

    std::string monitor_pipe_name_str = pipe_names::launcher_monitor();
    std::string executor_pipe_name_str = pipe_names::launcher_executor();
    std::string gui_pipe_name_str = pipe_names::launcher_gui();

    // C4 编码 helper（阶段 1.7）：
    //   原 std::wstring(str.begin(), str.end()) 仅对 ASCII 有效；
    //   utf8ToWide 保证非 ASCII 正确转换。
    std::wstring monitor_pipe_name = common::utf8ToWide(monitor_pipe_name_str);
    std::wstring executor_pipe_name = common::utf8ToWide(executor_pipe_name_str);
    std::wstring gui_pipe_name = common::utf8ToWide(gui_pipe_name_str);

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
            LOG_ERROR("Failed to launch " + common::wideToUtf8(info.name));
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

    g_monitor_pipe = &monitor_pipe;
    g_executor_pipe = &executor_pipe;
    g_gui_pipe = &gui_pipe;

    EventLoop event_loop;
    g_event_loop = &event_loop;

    for (auto& proc : g_managed_processes) {
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

    cleanup(g_managed_processes, job_handle);

    g_plugin_manager.reset();
    g_event_loop = nullptr;

    LOG_INFO("=== Launcher exited ===");

    Logger::instance().markCleanExit();

    return 0;
}