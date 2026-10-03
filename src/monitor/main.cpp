// src/monitor/main.cpp
#include "logger.h"
#include "pipe.h"
#include "process.h"
#include "constants.h"
#include "messages.h"
#include "message_router.h"
#include "init_list_utils.h"
#include "error_codes.h"
#include "error_notify_bridge.h"   // A.11.2b-1：ErrorContext → ErrorNotifyMessage 映射
#include "event_loop.h"
#include "common_utils.h"

// dm_signal（A.8 装配）
#include "signal_bus.h"
#include "message_bridge.h"
#include "signal_types.h"
#include "signal_strings.h"

#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <vector>
#include <atomic>
#include <tlhelp32.h>

#include "plugin_types.h"

using namespace dream_machine;
using namespace dream_machine::event;

namespace {

enum class SessionState {
    CREATING,
    RUNNING,
    SHUTTING_DOWN,
    CRASHED
};

enum class SessionEndReason {
    NORMAL_SHUTDOWN,
    CRASHED
};

// Session 用 shared_ptr 管理生命周期（保持不变）
struct Session {
    std::string session_id;
    HANDLE process_handle = nullptr;
    DWORD process_pid = 0;
    std::shared_ptr<NamedPipe> core_pipe;
    SessionState state = SessionState::CREATING;
    SessionEndReason end_reason = SessionEndReason::NORMAL_SHUTDOWN;
};

// ================================================================
// MonitorState：monitor 全局状态收敛载体（A.3.2 C3）
// ================================================================
struct MonitorState {
    // ---- 会话管理 ----
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions;
    std::mutex sessions_mutex;

    // ---- 进程资源 ----
    HANDLE monitor_job = nullptr;

    // ---- 通信资源 ----
    NamedPipe* launcher_pipe = nullptr;

    // ---- 生命周期 ----
    EventLoop* event_loop = nullptr;
    std::atomic<bool> should_stop{false};

    // ---- 消息分发表 ----
    MessageRouter message_router;

    // ---- 心跳 ----
    int heartbeat_counter = 0;
};

static MonitorState g_state;

// ----- 前向声明 -----
void handleFullSyncRequest(const std::string& payload, NamedPipe& launcher_pipe);
void broadcastShutdownToCoreEngines(const std::string& reason);
bool checkMaxSessionsReached();
void attachSignalBus();
void detachSignalBus();

// ================================================================
// publish_error：通过 SignalBus 发布结构化错误（A.11.2b-1 B3 接线）
//
// 单一映射路径（D5 §8.2 / D3 DREAM-010）：
//   ErrorContext 语义 → SignalPayload → SignalBus
//     ├→ Logger（进程内日志，[signal:error_notify] 前缀）
//     └→ MessageBridge → Sender 重建 ErrorNotifyMessage → launcher
//
// 说明：
//   - severity 走 error_severity_to_signal_level_int（受控映射）
//   - error_code 走 errorCodeToString（受控字符串）
//   - extra["error_code"] 携带错误码，供 Sender 重建 details
//   - 管道未连接时 Sender 返回 false；Logger 仍记录（进程内不丢）
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
    payload.extra["error_code"] = errorCodeToString(code);

    signal::SignalBus::instance().publish(payload);
}

// ================================================================
// dm_signal 装配（A.8 装配 + A.11.2b-1 Sender 完整实现）
//
// 订阅顺序（monitor）：Logger → MessageBridge
// 依据：详见文档13 §3.3
//
// 注：
//   - monitor 不使用 OperationTracker（详见文档5 §7.9）
//   - Sender 只处理 SGT_ERROR_NOTIFY（单一映射路径）
//   - SGT_SESSION_STATE_CHANGED / SGT_ENGINE_DIED 当前走直接 writeLine，
//     未来若需通过信号总线接入，再新增独立 Sender（保持单一职责）
// ================================================================
void attachSignalBus() {
    Logger::attach_to_signal_bus();
    signal::SignalBus::instance().subscribe(&signal::MessageBridge::instance());

    // monitor → launcher（错误通知）
    (void)signal::MessageBridge::instance().add_sender(
        "launcher",
        [](const signal::SignalPayload& payload) -> bool {
            if (!g_state.launcher_pipe || !g_state.launcher_pipe->isValid()) {
                return false;
            }

            // A.11.2b-1：从 SignalPayload 重建 ErrorNotifyMessage
            ErrorNotifyMessage notify;
            notify.source = "monitor";
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
            PipeResult result = g_state.launcher_pipe->writeLine(json);
            if (result == PipeResult::PIPE_OK) {
                LOG_INFO("ErrorNotify sent to launcher: " + payload.description);
                return true;
            }
            LOG_WARN("Failed to send ErrorNotify to launcher");
            return false;
        },
        [](const signal::SignalPayload& payload) -> bool {
            return payload.type == signal::SignalType::SGT_ERROR_NOTIFY;
        });

    LOG_INFO("dm_signal attached: Logger + MessageBridge (monitor)");
}

void detachSignalBus() {
    signal::MessageBridge::instance().remove_all_senders();
    Logger::detach_from_signal_bus();
    LOG_INFO("dm_signal detached (monitor)");
}

// ================================================================
// 发送会话状态变更
// ================================================================
void sendSessionStateToLauncher(NamedPipe& launcher_pipe,
                                const std::string& session_id,
                                const std::string& state,
                                const std::string& pipe_name = "") {
    SessionStateChangedMessage msg;
    msg.session_id = session_id;
    msg.state = state;
    if (!pipe_name.empty()) {
        msg.pipe_name = pipe_name;
    }
    std::string json = serializeSessionStateChanged(msg);
    (void)launcher_pipe.writeLine(json);
    LOG_INFO("Sent SESSION_STATE_CHANGED: " + session_id + " -> " + state);
}

// ================================================================
// 清理崩溃的会话（调用方须持 g_state.sessions_mutex）
//
// A.11.2b-1：崩溃清理时发布结构化错误（PROCESS_CRASHED）
// ================================================================
void cleanupCrashedSession(const std::string& session_id, NamedPipe& launcher_pipe) {
    auto it = g_state.sessions.find(session_id);
    if (it == g_state.sessions.end()) {
        return;
    }

    std::shared_ptr<Session> session = it->second;
    session->state = SessionState::CRASHED;
    session->end_reason = SessionEndReason::CRASHED;

    LOG_WARN("Session " + session_id + " crashed, cleaning up");

    // A.11.2b-1：结构化错误（进程内 + 跨进程）
    publish_error(ErrorCode::PROCESS_CRASHED,
                  ErrorSeverity::RECOVERABLE,
                  "core_engine crashed for session " + session_id,
                  "cleanupCrashedSession invoked",
                  session_id);

    if (session->core_pipe) {
        session->core_pipe->close();
    }

    if (session->process_handle && session->process_handle != INVALID_HANDLE_VALUE) {
        if (WaitForSingleObject(session->process_handle, 0) == WAIT_TIMEOUT) {
            TerminateProcess(session->process_handle, 1);
            WaitForSingleObject(session->process_handle, 1000);
        }
        CloseHandle(session->process_handle);
        session->process_handle = nullptr;
    }

    g_state.sessions.erase(it);
    sendSessionStateToLauncher(launcher_pipe, session_id, "crashed");
    LOG_INFO("Session " + session_id + " crash cleanup complete");
}

// ================================================================
// 检查会话数量是否达到上限
// ================================================================
bool checkMaxSessionsReached() {
    std::lock_guard<std::mutex> lock(g_state.sessions_mutex);
    size_t count = 0;
    for (const auto& pair : g_state.sessions) {
        if (pair.second->state == SessionState::RUNNING ||
            pair.second->state == SessionState::CREATING) {
            ++count;
        }
    }
    return count >= static_cast<size_t>(constants::MAX_SESSIONS);
}

// ================================================================
// 处理全量同步请求
// ================================================================
void handleFullSyncRequest(const std::string& payload, NamedPipe& launcher_pipe) {
    auto req = parseFullSyncRequest(payload);
    if (!req.has_value()) {
        LOG_WARN("Failed to parse FULL_SYNC_REQUEST");
        // A.11.2b-1：结构化错误
        publish_error(ErrorCode::JSON_ERROR,
                      ErrorSeverity::WARNING,
                      "Failed to parse FULL_SYNC_REQUEST",
                      "parseFullSyncRequest returned nullopt");
        return;
    }

    LOG_INFO("Full sync request received (request_id: " + std::to_string(req->request_id) + ")");

    FullSyncResponseMessage resp;
    resp.request_id = req->request_id;

    {
        std::lock_guard<std::mutex> lock(g_state.sessions_mutex);
        for (const auto& pair : g_state.sessions) {
            if (pair.second->state == SessionState::RUNNING) {
                SessionStateChangedMessage s;
                s.session_id = pair.second->session_id;
                s.state = "running";
                resp.sessions.push_back(s);
            }
        }
    }

    std::string resp_json = serializeFullSyncResponse(resp);
    if (launcher_pipe.writeLine(resp_json) == PipeResult::PIPE_OK) {
        LOG_INFO("Full sync response sent (" + std::to_string(resp.sessions.size()) + " sessions)");
    } else {
        LOG_ERROR("Failed to send full sync response");
        // A.11.2b-1：结构化错误
        publish_error(ErrorCode::PIPE_ERROR,
                      ErrorSeverity::RECOVERABLE,
                      "Failed to send FULL_SYNC_RESPONSE",
                      "writeLine to launcher_pipe failed");
    }
}

// ================================================================
// 下行广播 SHUTDOWN 给所有存活的 core_engine
//
// 依据 DREAM_MACHINE_CONCURRENCY_MODEL_BOUNDARY 专家裁决 Q3：
//   - 锁内收集 shared_ptr<NamedPipe> 列表
//   - 锁外逐个 writeLine
// ================================================================
void broadcastShutdownToCoreEngines(const std::string& reason) {
    std::vector<std::shared_ptr<NamedPipe>> targets;

    {
        std::lock_guard<std::mutex> lock(g_state.sessions_mutex);
        for (auto& pair : g_state.sessions) {
            Session& session = *pair.second;
            if (session.state != SessionState::RUNNING &&
                session.state != SessionState::SHUTTING_DOWN) {
                continue;
            }
            if (!session.core_pipe || !session.core_pipe->isValid()) {
                continue;
            }
            targets.push_back(session.core_pipe);
        }
    }

    LOG_INFO("Broadcasting SHUTDOWN to " + std::to_string(targets.size()) +
             " core_engine(s), reason=" + reason);

    ShutdownMessage msg;
    msg.reason = reason;
    msg.initiator = shutdown_initiator::MONITOR;

    std::string json = serializeShutdown(msg);

    for (auto& pipe : targets) {
        PipeResult result = pipe->writeLine(json);
        if (result != PipeResult::PIPE_OK) {
            LOG_WARN("Failed to send SHUTDOWN to core_engine, result=" +
                     std::to_string(static_cast<int>(result)));
        }
    }

    LOG_INFO("SHUTDOWN broadcast complete");
}

// ================================================================
// 消息 handler 注册
// ================================================================
void registerMessageHandlers(MessageRouter& router) {
    // ---- SHUTDOWN ----
    router.register_handler(msg_types::SHUTDOWN,
        [](const std::string& payload, void* /*ctx*/) {
            auto shutdown_msg = parseShutdown(payload);
            std::string reason = shutdown_msg.has_value()
                                 ? shutdown_msg->reason
                                 : std::string(shutdown_reason::PEER_EXIT);

            LOG_INFO("Received SHUTDOWN from launcher, reason=" + reason);

            broadcastShutdownToCoreEngines(reason);

            g_state.should_stop = true;
            if (g_state.event_loop) {
                g_state.event_loop->stop();
            }
        });

    // ---- INIT_LIST ----
    router.register_handler(msg_types::INIT_LIST,
        [](const std::string& payload, void* ctx) {
            auto* pipe = static_cast<NamedPipe*>(ctx);
            if (!pipe) return;

            if (!init_list_utils::processInitList(payload)) {
                return;
            }

            InitListAckMessage ack;
            ack.status = "ok";
            std::string ack_json = serializeInitListAck(ack);
            pipe->writeLine(ack_json);
            LOG_INFO("Sent INIT_LIST_ACK");
        });

    // ---- REQUEST_ENGINE ----（A1 未实现，保留占位）
    router.register_handler(msg_types::REQUEST_ENGINE,
        [](const std::string& /*payload*/, void* ctx) {
            auto* pipe = static_cast<NamedPipe*>(ctx);
            if (!pipe) return;

            LOG_WARN("REQUEST_ENGINE not yet implemented");
            if (checkMaxSessionsReached()) {
                LOG_WARN("Max sessions reached, rejecting REQUEST_ENGINE");
                EngineFailedMessage fail_msg;
                fail_msg.session_id = "unknown";
                fail_msg.reason = "max_sessions_reached";
                std::string fail_json = serializeEngineFailed(fail_msg);
                pipe->writeLine(fail_json);

                // A.11.2b-1：结构化错误
                publish_error(ErrorCode::SESSION_MAX_REACHED,
                              ErrorSeverity::WARNING,
                              "Rejected REQUEST_ENGINE: max sessions reached",
                              "checkMaxSessionsReached returned true");
            }
        });

    // ---- FULL_SYNC_REQUEST ----
    router.register_handler(msg_types::FULL_SYNC_REQUEST,
        [](const std::string& payload, void* ctx) {
            auto* pipe = static_cast<NamedPipe*>(ctx);
            if (!pipe) return;
            handleFullSyncRequest(payload, *pipe);
        });

    // ---- MONITOR_GET_ACTIVE_SESSIONS ----
    router.register_handler(msg_types::MONITOR_GET_ACTIVE_SESSIONS,
        [](const std::string& /*payload*/, void* ctx) {
            auto* pipe = static_cast<NamedPipe*>(ctx);
            if (!pipe) return;

            FullSyncResponseMessage resp_msg;
            resp_msg.request_id = 0;
            {
                std::lock_guard<std::mutex> lock(g_state.sessions_mutex);
                for (const auto& pair : g_state.sessions) {
                    if (pair.second->state == SessionState::RUNNING) {
                        SessionStateChangedMessage s;
                        s.session_id = pair.second->session_id;
                        s.state = "running";
                        resp_msg.sessions.push_back(s);
                    }
                }
            }
            std::string response = serializeFullSyncResponse(resp_msg);
            (void)pipe->writeLine(response);
            LOG_INFO("ACTIVE_SESSIONS_RESP sent");
        });
}

// ================================================================
// 处理 launcher 消息（事件驱动回调）
//
// A.11.2b-1：isBroken / 解析失败 → 结构化错误
// ================================================================
void processLauncherMessage(EventType type, void* user_data) {
    (void)type;
    if (!g_state.launcher_pipe || g_state.should_stop) {
        return;
    }

    NamedPipe& launcher_pipe = *g_state.launcher_pipe;

    if (launcher_pipe.isBroken()) {
        LOG_INFO("Launcher pipe broken, stopping event loop");
        publish_error(ErrorCode::PIPE_BROKEN,
                      ErrorSeverity::FATAL,
                      "Launcher pipe broken",
                      "isBroken() returned true; stopping event loop");
        if (g_state.event_loop) {
            g_state.event_loop->stop();
        }
        return;
    }

    DWORD bytes_available = 0;
    PipeResult peek_result = launcher_pipe.peekAvailable(bytes_available);

    if (peek_result == PipeResult::PIPE_BROKEN) {
        LOG_INFO("Launcher pipe broken (peek), stopping event loop");
        publish_error(ErrorCode::PIPE_BROKEN,
                      ErrorSeverity::FATAL,
                      "Launcher pipe broken (peek)",
                      "peekAvailable returned PIPE_BROKEN");
        if (g_state.event_loop) {
            g_state.event_loop->stop();
        }
        return;
    }

    if (peek_result != PipeResult::PIPE_OK || bytes_available == 0) {
        return;
    }

    std::string message;
    PipeResult read_result = launcher_pipe.readLineBuffered(message, 3000);

    if (read_result == PipeResult::PIPE_OK) {
        LOG_INFO("From launcher: " + message);

        std::string type_str, cmd, payload;
        if (parseBaseMessage(message, type_str, cmd, payload)) {
            if (!g_state.message_router.dispatch(type_str, payload, &launcher_pipe)) {
                LOG_WARN("Unhandled message type: " + type_str);
            }
        } else {
            LOG_WARN("Failed to parse base message");
            publish_error(ErrorCode::JSON_ERROR,
                          ErrorSeverity::WARNING,
                          "Failed to parse base message from launcher",
                          "parseBaseMessage returned false");
        }

    } else if (read_result == PipeResult::PIPE_BROKEN) {
        LOG_INFO("Launcher pipe broken (read), stopping event loop");
        publish_error(ErrorCode::PIPE_BROKEN,
                      ErrorSeverity::FATAL,
                      "Launcher pipe broken (read)",
                      "readLineBuffered returned PIPE_BROKEN");
        if (g_state.event_loop) {
            g_state.event_loop->stop();
        }
    } else if (read_result == PipeResult::PIPE_TIMEOUT) {
        LOG_WARN("Read timeout, will retry");
    }
}

// ================================================================
// 轮询 core_engine 管道（定时回调）
// ================================================================
void pollCorePipes(EventType type, void* user_data) {
    (void)type;
    if (!g_state.launcher_pipe || g_state.should_stop) {
        return;
    }

    NamedPipe& launcher_pipe = *g_state.launcher_pipe;

    std::lock_guard<std::mutex> lock(g_state.sessions_mutex);

    std::vector<std::string> crashed_sessions;
    for (auto& pair : g_state.sessions) {
        Session& session = *pair.second;
        if (session.state != SessionState::RUNNING &&
            session.state != SessionState::CREATING) {
            continue;
        }
        if (session.core_pipe && session.core_pipe->isBroken()) {
            LOG_WARN("core_engine pipe broken for session: " + session.session_id);
            crashed_sessions.push_back(session.session_id);
        }
    }

    for (const auto& session_id : crashed_sessions) {
        cleanupCrashedSession(session_id, launcher_pipe);
    }
}

// ================================================================
// 心跳日志（定时回调）
// ================================================================
void logHeartbeat(EventType type, void* user_data) {
    (void)type;
    (void)user_data;

    ++g_state.heartbeat_counter;
    if (g_state.heartbeat_counter % 100 == 0) {
        std::lock_guard<std::mutex> lock(g_state.sessions_mutex);
        LOG_INFO("Monitor heartbeat: " + std::to_string(g_state.heartbeat_counter) +
                 " iterations, active sessions: " + std::to_string(g_state.sessions.size()));
    }
}

} // namespace

// ================================================================
// main 入口
//
// 本分片合并改动：
//   A.3.2      C3 全局状态收敛（MonitorState g_state）
//   A.8        dm_signal 装配（Logger + MessageBridge）
//   A.11.2b-1  B3 错误结构化接线（Sender 完整实现 + isBroken 结构化错误）
// ================================================================
int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("monitor");
    Logger::instance().setLogDirectory(common::pathFromRoot("logs"));

    const bool archived_prev = Logger::instance().archiveLastSessionIfDirty();

    LOG_INFO("=== Dream Machine Monitor starting ===");

    if (archived_prev) {
        LOG_INFO("Previous session logs archived to logs/crashes/");
    }

    // A.8：dm_signal 装配
    attachSignalBus();

    registerMessageHandlers(g_state.message_router);

    std::string parent_pid_str = common::getArgValue(argc, argv, "--parent-pid");
    DWORD expected_parent_pid = 0;
    if (!parent_pid_str.empty()) {
        expected_parent_pid = static_cast<DWORD>(std::stoul(parent_pid_str));
    }

    if (!common::verifyParentPid(expected_parent_pid)) {
        // A.11.2b-1：管道未连接，Sender 会失败；Logger 仍会记录信号
        publish_error(ErrorCode::PROCESS_LAUNCH_FAILED,
                      ErrorSeverity::FATAL,
                      "Parent PID verification failed",
                      "verifyParentPid returned false");
        detachSignalBus();
        return 1;
    }

    g_state.monitor_job = CreateJobObjectW(nullptr, L"Global\\DreamMachine_Monitor_Job");
    if (!g_state.monitor_job) {
        LOG_ERROR("Failed to create Monitor Job Object: error " + std::to_string(GetLastError()));
        publish_error(ErrorCode::PROCESS_LAUNCH_FAILED,
                      ErrorSeverity::FATAL,
                      "Failed to create Monitor Job Object",
                      "CreateJobObjectW returned null");
    } else {
        LOG_INFO("Monitor Job Object created successfully");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
        job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(g_state.monitor_job, JobObjectExtendedLimitInformation,
                                     &job_info, sizeof(job_info))) {
            LOG_ERROR("Failed to configure Monitor Job Object: error " + std::to_string(GetLastError()));
            publish_error(ErrorCode::PROCESS_LAUNCH_FAILED,
                          ErrorSeverity::FATAL,
                          "Failed to configure Monitor Job Object",
                          "SetInformationJobObject failed");
        } else {
            LOG_INFO("Monitor Job Object configured: KILL_ON_JOB_CLOSE enabled");
        }
    }

    std::string pipe_name_str = pipe_names::launcher_monitor();
    std::wstring pipe_name = common::utf8ToWide(pipe_name_str);

    LOG_INFO("Connecting to launcher pipe: " + pipe_name_str);

    NamedPipe launcher_pipe;
    if (!launcher_pipe.connect(pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to launcher pipe, exiting");
        // A.11.2b-1：管道未连接，Sender 会失败；Logger 仍记录信号
        publish_error(ErrorCode::PIPE_CONNECT_FAILED,
                      ErrorSeverity::FATAL,
                      "Failed to connect to launcher pipe",
                      pipe_name_str);
        if (g_state.monitor_job) CloseHandle(g_state.monitor_job);
        detachSignalBus();
        return 1;
    }

    LOG_INFO("Connected to launcher pipe");

    RegisterMessage reg_msg;
    reg_msg.process = "monitor";
    std::string register_msg = serializeRegister(reg_msg);

    if (launcher_pipe.writeLine(register_msg) != PipeResult::PIPE_OK) {
        LOG_ERROR("Failed to send registration message, exiting");
        publish_error(ErrorCode::PIPE_ERROR,
                      ErrorSeverity::FATAL,
                      "Failed to send registration message to launcher",
                      "writeLine returned non-PIPE_OK");
        if (g_state.monitor_job) CloseHandle(g_state.monitor_job);
        detachSignalBus();
        return 1;
    }
    LOG_INFO("Registration message sent: " + register_msg);

    g_state.launcher_pipe = &launcher_pipe;

    EventLoop event_loop;
    g_state.event_loop = &event_loop;

    EventHandle read_handle = event_loop.registerReadable(
        launcher_pipe.getHandle(), processLauncherMessage, nullptr);
    if (!read_handle.active) {
        LOG_ERROR("Failed to register launcher pipe readable event");
        publish_error(ErrorCode::PIPE_ERROR,
                      ErrorSeverity::FATAL,
                      "Failed to register launcher pipe readable event",
                      "registerReadable returned inactive handle");
        detachSignalBus();
        return 1;
    }

    EventHandle poll_handle = event_loop.registerTimer(500, pollCorePipes, nullptr, false);
    if (!poll_handle.active) {
        LOG_ERROR("Failed to register core pipe polling timer");
        publish_error(ErrorCode::PROCESS_ERROR,
                      ErrorSeverity::FATAL,
                      "Failed to register core pipe polling timer",
                      "registerTimer returned inactive handle");
        detachSignalBus();
        return 1;
    }

    EventHandle heartbeat_handle = event_loop.registerTimer(100, logHeartbeat, nullptr, false);
    if (!heartbeat_handle.active) {
        LOG_ERROR("Failed to register heartbeat timer");
        publish_error(ErrorCode::PROCESS_ERROR,
                      ErrorSeverity::FATAL,
                      "Failed to register heartbeat timer",
                      "registerTimer returned inactive handle");
        detachSignalBus();
        return 1;
    }

    EventHandle stop_signal = event_loop.registerSignal([](EventType type, void* data) {
        (void)type;
        (void)data;
        LOG_INFO("Stop signal received");
        g_state.should_stop = true;
    });
    if (!stop_signal.active) {
        LOG_WARN("Failed to register stop signal");
    }

    LOG_INFO("Entering event-driven main loop...");
    event_loop.run();

    LOG_INFO("Shutting down monitor...");

    event_loop.unregister(read_handle);
    event_loop.unregister(poll_handle);
    event_loop.unregister(heartbeat_handle);
    event_loop.unregister(stop_signal);

    {
        std::lock_guard<std::mutex> lock(g_state.sessions_mutex);
        for (auto& pair : g_state.sessions) {
            std::shared_ptr<Session> session = pair.second;
            LOG_INFO("Cleaning up session: " + session->session_id);

            if (session->core_pipe) {
                session->core_pipe->close();
            }

            if (session->process_handle && session->process_handle != INVALID_HANDLE_VALUE) {
                if (WaitForSingleObject(session->process_handle, 0) == WAIT_TIMEOUT) {
                    LOG_INFO("Terminating core_engine (PID: " + std::to_string(session->process_pid) +
                             ") for session " + session->session_id);
                    TerminateProcess(session->process_handle, 1);
                    WaitForSingleObject(session->process_handle, 1000);
                }
                CloseHandle(session->process_handle);
                session->process_handle = nullptr;
            }
        }
        g_state.sessions.clear();
    }

    if (g_state.monitor_job) {
        LOG_INFO("Closing Monitor Job Object...");
        CloseHandle(g_state.monitor_job);
        g_state.monitor_job = nullptr;
    }

    launcher_pipe.close();
    g_state.launcher_pipe = nullptr;
    g_state.event_loop = nullptr;

    // A.8：dm_signal 卸载
    detachSignalBus();

    LOG_INFO("=== Monitor exited ===");

    Logger::instance().markCleanExit();

    return 0;
}