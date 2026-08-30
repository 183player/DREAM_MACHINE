// src/monitor/main.cpp
#include "logger.h"
#include "pipe.h"
#include "process.h"
#include "constants.h"
#include "messages.h"
#include "error_codes.h"
#include "event_loop.h"
#include "common_utils.h"

#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <unordered_map>
#include <mutex>
#include <tlhelp32.h>
#include <vector>
#include <atomic>

#include "plugin_types.h"

using namespace dream_machine;
using namespace dream_machine::event;
using namespace dream_machine::common;

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

struct Session {
    std::string session_id;
    HANDLE process_handle = nullptr;
    DWORD process_pid = 0;
    NamedPipe core_pipe;
    SessionState state = SessionState::CREATING;
    SessionEndReason end_reason = SessionEndReason::NORMAL_SHUTDOWN;
};

std::unordered_map<std::string, Session> sessions_;
std::mutex sessions_mutex_;
HANDLE g_monitor_job_ = nullptr;

NamedPipe* g_launcher_pipe = nullptr;
EventLoop* g_event_loop = nullptr;
std::atomic<bool> g_should_stop{false};

// ----- 发送会话状态变更（使用结构化序列化） -----
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

// ----- 处理 INIT_LIST -----
void handleInitList(const std::string& payload, NamedPipe& launcher_pipe) {
    LOG_INFO("Processing INIT_LIST...");

    auto list = plugin::initListFromJson(payload);
    if (!list.has_value()) {
        LOG_ERROR("Failed to parse INIT_LIST payload");
        return;
    }

    for (const auto& entry : list->entries) {
        if (entry.type == plugin::ModificationType::REPLACE) {
            LOG_INFO("REPLACE: target=" + entry.target_file + ", winner=" + entry.winner_plugin_id);
        } else if (entry.type == plugin::ModificationType::EXTEND) {
            LOG_INFO("EXTEND: container=" + entry.container_id + ", plugin=" + entry.plugin_id);
        }
        if (!entry.rule_file.empty()) {
            LOG_INFO("  rule_file: " + entry.rule_file);
        }
        if (!entry.trigger.empty()) {
            LOG_INFO("  trigger: " + entry.trigger);
        }
    }

    LOG_INFO("INIT_LIST processing complete");
    InitListAckMessage ack;
    ack.status = "ok";
    std::string ack_json = serializeInitListAck(ack);
    (void)launcher_pipe.writeLine(ack_json);
    LOG_INFO("Sent INIT_LIST_ACK");
}

// ----- 清理崩溃的会话 -----
void cleanupCrashedSession(const std::string& session_id, NamedPipe& launcher_pipe) {
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return;
    }

    Session& session = it->second;
    session.state = SessionState::CRASHED;
    session.end_reason = SessionEndReason::CRASHED;

    LOG_WARN("Session " + session_id + " crashed, cleaning up");
    session.core_pipe.close();

    if (session.process_handle && session.process_handle != INVALID_HANDLE_VALUE) {
        if (WaitForSingleObject(session.process_handle, 0) == WAIT_TIMEOUT) {
            TerminateProcess(session.process_handle, 1);
            WaitForSingleObject(session.process_handle, 1000);
        }
        CloseHandle(session.process_handle);
        session.process_handle = nullptr;
    }

    sessions_.erase(it);
    sendSessionStateToLauncher(launcher_pipe, session_id, "crashed");
    LOG_INFO("Session " + session_id + " crash cleanup complete");
}

// ----- 检查会话数量是否达到上限 -----
bool checkMaxSessionsReached() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    size_t count = 0;
    for (const auto& pair : sessions_) {
        if (pair.second.state == SessionState::RUNNING ||
            pair.second.state == SessionState::CREATING) {
            ++count;
        }
    }
    return count >= static_cast<size_t>(constants::MAX_SESSIONS);
}

// ----- 处理全量同步请求 -----
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
            if (pair.second.state == SessionState::RUNNING) {
                SessionStateChangedMessage s;
                s.session_id = pair.second.session_id;
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
            if (type_str == msg_types::INIT_LIST) {
                handleInitList(payload, launcher_pipe);
            } else if (type_str == msg_types::REQUEST_ENGINE) {
                LOG_WARN("REQUEST_ENGINE not yet implemented");
                if (checkMaxSessionsReached()) {
                    LOG_WARN("Max sessions reached, rejecting REQUEST_ENGINE");
                    EngineFailedMessage fail_msg;
                    fail_msg.session_id = "unknown";
                    fail_msg.reason = "max_sessions_reached";
                    std::string fail_json = serializeEngineFailed(fail_msg);
                    launcher_pipe.writeLine(fail_json);
                }
            } else if (type_str == msg_types::FULL_SYNC_REQUEST) {
                handleFullSyncRequest(payload, launcher_pipe);
            } else if (type_str == msg_types::MONITOR_GET_ACTIVE_SESSIONS) {
                FullSyncResponseMessage resp_msg;
                resp_msg.request_id = 0;
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                for (const auto& pair : sessions_) {
                    if (pair.second.state == SessionState::RUNNING) {
                        SessionStateChangedMessage s;
                        s.session_id = pair.second.session_id;
                        s.state = "running";
                        resp_msg.sessions.push_back(s);
                    }
                }
                std::string response = serializeFullSyncResponse(resp_msg);
                (void)launcher_pipe.writeLine(response);
                LOG_INFO("ACTIVE_SESSIONS_RESP sent");
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
        Session& session = pair.second;
        if (session.state != SessionState::RUNNING &&
            session.state != SessionState::CREATING) {
            continue;
        }
        if (session.core_pipe.isBroken()) {
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
// ================================================================
int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("monitor");
    LOG_INFO("=== Dream Machine Monitor starting ===");

    // 使用 common_utils 解析参数并验证父进程
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
    std::wstring pipe_name(pipe_name_str.begin(), pipe_name_str.end());

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

    // ============================================================
    // 初始化事件循环
    // ============================================================
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

    // ============================================================
    // 清理
    // ============================================================
    LOG_INFO("Shutting down monitor...");

    event_loop.unregister(read_handle);
    event_loop.unregister(poll_handle);
    event_loop.unregister(heartbeat_handle);
    event_loop.unregister(stop_signal);

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto& pair : sessions_) {
            Session& session = pair.second;
            LOG_INFO("Cleaning up session: " + session.session_id);
            session.core_pipe.close();

            if (session.process_handle && session.process_handle != INVALID_HANDLE_VALUE) {
                if (WaitForSingleObject(session.process_handle, 0) == WAIT_TIMEOUT) {
                    LOG_INFO("Terminating core_engine (PID: " + std::to_string(session.process_pid) +
                             ") for session " + session.session_id);
                    TerminateProcess(session.process_handle, 1);
                    WaitForSingleObject(session.process_handle, 1000);
                }
                CloseHandle(session.process_handle);
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
    return 0;
}