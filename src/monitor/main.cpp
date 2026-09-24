// src/monitor/main.cpp
#include "logger.h"
#include "pipe.h"
#include "process.h"
#include "constants.h"
#include "messages.h"
#include "message_router.h"
#include "init_list_utils.h"
#include "error_codes.h"
#include "event_loop.h"
#include "common_utils.h"

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
// 注：不再 using dream_machine::common——
//     新增的 utf8ToWide / wideToUtf8 / pathFromRoot 用 common:: 前缀显式调用。

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

// Session 用 shared_ptr 管理生命周期：
//   - sessions_ 存储 shared_ptr<Session>
//   - core_pipe 存储 shared_ptr<NamedPipe>
//   - 广播 SHUTDOWN 时锁内收集 shared_ptr，锁外发送；
//     即使会话在发送期间被 erase，pipe 对象仍存活至发送完成
struct Session {
    std::string session_id;
    HANDLE process_handle = nullptr;
    DWORD process_pid = 0;
    std::shared_ptr<NamedPipe> core_pipe;
    SessionState state = SessionState::CREATING;
    SessionEndReason end_reason = SessionEndReason::NORMAL_SHUTDOWN;
};

std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
std::mutex sessions_mutex_;
HANDLE g_monitor_job_ = nullptr;

NamedPipe* g_launcher_pipe = nullptr;
EventLoop* g_event_loop = nullptr;
std::atomic<bool> g_should_stop{false};

// 消息分发表
//
// 迁移了全部 5 个 handler（SHUTDOWN / INIT_LIST / REQUEST_ENGINE /
// FULL_SYNC_REQUEST / MONITOR_GET_ACTIVE_SESSIONS）。
// 无 fallback 特例——所有消息类型统一走 router.dispatch。
//
// 回退方式：删除本变量 + registerMessageHandlers 调用，
//          并将 processLauncherMessage 恢复为原始 if-else 即可。
MessageRouter g_message_router;

// ----- 前向声明 -----
void handleFullSyncRequest(const std::string& payload, NamedPipe& launcher_pipe);
void broadcastShutdownToCoreEngines(const std::string& reason);
bool checkMaxSessionsReached();

// ================================================================
// 发送会话状态变更（使用结构化序列化）
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
// 清理崩溃的会话（调用方须持 sessions_mutex_）
// ================================================================
void cleanupCrashedSession(const std::string& session_id, NamedPipe& launcher_pipe) {
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return;
    }

    std::shared_ptr<Session> session = it->second;
    session->state = SessionState::CRASHED;
    session->end_reason = SessionEndReason::CRASHED;

    LOG_WARN("Session " + session_id + " crashed, cleaning up");

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

    sessions_.erase(it);
    sendSessionStateToLauncher(launcher_pipe, session_id, "crashed");
    LOG_INFO("Session " + session_id + " crash cleanup complete");
}

// ================================================================
// 检查会话数量是否达到上限
// ================================================================
bool checkMaxSessionsReached() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    size_t count = 0;
    for (const auto& pair : sessions_) {
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
        return;
    }

    LOG_INFO("Full sync request received (request_id: " + std::to_string(req->request_id) + ")");

    FullSyncResponseMessage resp;
    resp.request_id = req->request_id;

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (const auto& pair : sessions_) {
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
    }
}

// ================================================================
// 下行广播 SHUTDOWN 给所有存活的 core_engine
//
// 依据 DREAM_MACHINE_CONCURRENCY_MODEL_BOUNDARY 专家裁决 Q3：
//   - 锁内收集 shared_ptr<NamedPipe> 列表
//   - 锁外逐个 writeLine
//   - 即使会话在发送期间被 erase，pipe 对象仍由 shared_ptr 保活
//
// 筛选条件：状态 ∈ {RUNNING, SHUTTING_DOWN} 且 pipe 有效
// 写入失败仅 WARN，不重试，不阻塞
// ================================================================
void broadcastShutdownToCoreEngines(const std::string& reason) {
    std::vector<std::shared_ptr<NamedPipe>> targets;

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto& pair : sessions_) {
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

            g_should_stop = true;
            if (g_event_loop) {
                g_event_loop->stop();
            }
        });

    // ---- INIT_LIST ----
    // 使用公共骨架 processInitList；monitor 无进程特有钩子
    router.register_handler(msg_types::INIT_LIST,
        [](const std::string& payload, void* ctx) {
            auto* pipe = static_cast<NamedPipe*>(ctx);
            if (!pipe) return;

            if (!init_list_utils::processInitList(payload)) {
                return;  // 解析失败，不发 ACK
            }

            InitListAckMessage ack;
            ack.status = "ok";
            std::string ack_json = serializeInitListAck(ack);
            pipe->writeLine(ack_json);
            LOG_INFO("Sent INIT_LIST_ACK");
        });

    // ---- REQUEST_ENGINE ----
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
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                for (const auto& pair : sessions_) {
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
// ================================================================
void processLauncherMessage(EventType type, void* user_data) {
    (void)type;
    if (!g_launcher_pipe || g_should_stop) {
        return;
    }

    NamedPipe& launcher_pipe = *g_launcher_pipe;

    if (launcher_pipe.isBroken()) {
        LOG_INFO("Launcher pipe broken, stopping event loop");
        if (g_event_loop) {
            g_event_loop->stop();
        }
        return;
    }

    DWORD bytes_available = 0;
    PipeResult peek_result = launcher_pipe.peekAvailable(bytes_available);

    if (peek_result == PipeResult::PIPE_BROKEN) {
        LOG_INFO("Launcher pipe broken (peek), stopping event loop");
        if (g_event_loop) {
            g_event_loop->stop();
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
            if (!g_message_router.dispatch(type_str, payload, &launcher_pipe)) {
                LOG_WARN("Unhandled message type: " + type_str);
            }
        } else {
            LOG_WARN("Failed to parse base message");
        }

    } else if (read_result == PipeResult::PIPE_BROKEN) {
        LOG_INFO("Launcher pipe broken (read), stopping event loop");
        if (g_event_loop) {
            g_event_loop->stop();
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
    if (!g_launcher_pipe || g_should_stop) {
        return;
    }

    NamedPipe& launcher_pipe = *g_launcher_pipe;

    std::lock_guard<std::mutex> lock(sessions_mutex_);

    std::vector<std::string> crashed_sessions;
    for (auto& pair : sessions_) {
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
int g_heartbeat_counter = 0;

void logHeartbeat(EventType type, void* user_data) {
    (void)type;
    (void)user_data;

    ++g_heartbeat_counter;
    if (g_heartbeat_counter % 100 == 0) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        LOG_INFO("Monitor heartbeat: " + std::to_string(g_heartbeat_counter) +
                 " iterations, active sessions: " + std::to_string(sessions_.size()));
    }
}

} // namespace

// ================================================================
// main 入口
//
// 路径策略（Step 0 路径修正）：
//   所有运行时资源基于可执行文件所在目录，不依赖 CWD。
//
// 日志生命周期：
//   - 启动：setProcessName → setLogDirectory → archiveLastSessionIfDirty → 开始日志
//   - 退出：最后一条日志 → markCleanExit → return 0
//
// 失败路径（父进程校验、连接、注册、事件注册失败）不写 .clean_exit：
// 它们不是正常会话，下次启动时应被识别为异常退出并归档。
// ================================================================
int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("monitor");

    // 路径修正：日志目录基于 exe 目录，不依赖 CWD
    Logger::instance().setLogDirectory(common::pathFromRoot("logs"));

    const bool archived_prev = Logger::instance().archiveLastSessionIfDirty();

    LOG_INFO("=== Dream Machine Monitor starting ===");

    if (archived_prev) {
        LOG_INFO("Previous session logs archived to logs/crashes/");
    }

    registerMessageHandlers(g_message_router);

    std::string parent_pid_str = common::getArgValue(argc, argv, "--parent-pid");
    DWORD expected_parent_pid = 0;
    if (!parent_pid_str.empty()) {
        expected_parent_pid = static_cast<DWORD>(std::stoul(parent_pid_str));
    }

    if (!common::verifyParentPid(expected_parent_pid)) {
        return 1;
    }

    g_monitor_job_ = CreateJobObjectW(nullptr, L"Global\\DreamMachine_Monitor_Job");
    if (!g_monitor_job_) {
        LOG_ERROR("Failed to create Monitor Job Object: error " + std::to_string(GetLastError()));
    } else {
        LOG_INFO("Monitor Job Object created successfully");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
        job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(g_monitor_job_, JobObjectExtendedLimitInformation,
                                     &job_info, sizeof(job_info))) {
            LOG_ERROR("Failed to configure Monitor Job Object: error " + std::to_string(GetLastError()));
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
        if (g_monitor_job_) CloseHandle(g_monitor_job_);
        return 1;
    }

    LOG_INFO("Connected to launcher pipe");

    RegisterMessage reg_msg;
    reg_msg.process = "monitor";
    std::string register_msg = serializeRegister(reg_msg);

    if (launcher_pipe.writeLine(register_msg) != PipeResult::PIPE_OK) {
        LOG_ERROR("Failed to send registration message, exiting");
        if (g_monitor_job_) CloseHandle(g_monitor_job_);
        return 1;
    }
    LOG_INFO("Registration message sent: " + register_msg);

    g_launcher_pipe = &launcher_pipe;

    EventLoop event_loop;
    g_event_loop = &event_loop;

    EventHandle read_handle = event_loop.registerReadable(
        launcher_pipe.getHandle(),
        processLauncherMessage,
        nullptr
    );
    if (!read_handle.active) {
        LOG_ERROR("Failed to register launcher pipe readable event");
        return 1;
    }

    EventHandle poll_handle = event_loop.registerTimer(
        500,
        pollCorePipes,
        nullptr,
        false
    );
    if (!poll_handle.active) {
        LOG_ERROR("Failed to register core pipe polling timer");
        return 1;
    }

    EventHandle heartbeat_handle = event_loop.registerTimer(
        100,
        logHeartbeat,
        nullptr,
        false
    );
    if (!heartbeat_handle.active) {
        LOG_ERROR("Failed to register heartbeat timer");
        return 1;
    }

    EventHandle stop_signal = event_loop.registerSignal([](EventType type, void* data) {
        (void)type;
        (void)data;
        LOG_INFO("Stop signal received");
        g_should_stop = true;
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
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto& pair : sessions_) {
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
        sessions_.clear();
    }

    if (g_monitor_job_) {
        LOG_INFO("Closing Monitor Job Object...");
        CloseHandle(g_monitor_job_);
        g_monitor_job_ = nullptr;
    }

    launcher_pipe.close();

    g_launcher_pipe = nullptr;
    g_event_loop = nullptr;

    LOG_INFO("=== Monitor exited ===");

    Logger::instance().markCleanExit();

    return 0;
}