// src/monitor/main.cpp
#include "logger.h"
#include "pipe.h"
#include "process.h"
#include "constants.h"
#include "messages.h"

#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <unordered_map>
#include <mutex>
#include <tlhelp32.h>
#include <vector>

#include "plugin_types.h"

using namespace dream_machine;

// ================================================================
// 内部辅助（匿名命名空间）
// ================================================================
namespace {

// 会话状态枚举（内部链接）
enum class SessionState {
    CREATING,
    RUNNING,
    SHUTTING_DOWN,
    CRASHED
};

// 会话退出原因（内部链接）
enum class SessionEndReason {
    NORMAL_SHUTDOWN,
    CRASHED
    // TIMEOUT 已移除（未使用）
};

// 会话结构（内部链接）
struct Session {
    std::string session_id;
    HANDLE process_handle = nullptr;
    DWORD process_pid = 0;
    NamedPipe core_pipe;
    SessionState state = SessionState::CREATING;
    SessionEndReason end_reason = SessionEndReason::NORMAL_SHUTDOWN;
    // create_time 已移除（未使用）
};

std::unordered_map<std::string, Session> sessions_;
std::mutex sessions_mutex_;
HANDLE g_monitor_job_ = nullptr;

// 命令行参数解析
std::string getArgValue(int argc, char* argv[], const std::string& key) {
    for (int i = 1; i < argc - 1; ++i) {
        if (argv[i] == key) {
            return argv[i + 1];
        }
    }
    return {};
}

// 验证父进程 PID
bool verifyParentPid(DWORD expected_parent_pid) {
    if (expected_parent_pid == 0) {
        LOG_ERROR("Missing --parent-pid argument, refusing to run standalone");
        return false;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        LOG_ERROR("CreateToolhelp32Snapshot failed: error " + std::to_string(GetLastError()));
        return false;
    }

    PROCESSENTRY32W pe = {sizeof(PROCESSENTRY32W)};
    DWORD current_pid = GetCurrentProcessId();
    DWORD real_parent_pid = 0;

    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (pe.th32ProcessID == current_pid) {
                real_parent_pid = pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &pe));
    }

    CloseHandle(snapshot);

    if (real_parent_pid == 0) {
        LOG_ERROR("Failed to determine real parent PID");
        return false;
    }

    if (real_parent_pid != expected_parent_pid) {
        LOG_ERROR("Parent PID mismatch: expected " + std::to_string(expected_parent_pid) +
                  ", actual " + std::to_string(real_parent_pid) + ", refusing to run");
        return false;
    }

    LOG_INFO("Parent PID verification passed (PID: " + std::to_string(real_parent_pid) + ")");
    return true;
}

// 发送会话状态变更给 launcher（忽略写入结果，避免警告）
void sendSessionStateToLauncher(NamedPipe& launcher_pipe,
                                const std::string& session_id,
                                const std::string& state,
                                const std::string& pipe_name = "") {
    std::string msg;
    msg += R"({"type":"session_state","cmd":"SESSION_STATE_CHANGED","payload":{")";
    msg += R"("session_id":")" + session_id + R"(",)" ;
    msg += R"("state":")" + state + R"(")";
    if (!pipe_name.empty()) {
        msg += R"(,"pipe_name":")" + pipe_name + R"(")";
    }
    msg += "}}";

    (void)launcher_pipe.writeLine(msg);  // 忽略返回值，消除警告
    LOG_INFO("Sent SESSION_STATE_CHANGED: " + session_id + " -> " + state);
}

// 清理会话（直接内联到处理中，此函数已移除，使用内联清理）

// ================================================================
// 处理 INIT_LIST 消息
// ================================================================
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
    // 发送 ACK（内部写入，返回值忽略）
    InitListAckMessage ack;
    ack.status = "ok";
    std::string ack_json = serializeInitListAck(ack);
    (void)launcher_pipe.writeLine(ack_json);
    LOG_INFO("Sent INIT_LIST_ACK");
}

// 清理崩溃的会话（内联函数）
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

} // namespace

// ================================================================
// main 入口
// ================================================================
int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("monitor");
    LOG_INFO("=== Dream Machine Monitor starting ===");

    std::string parent_pid_str = getArgValue(argc, argv, "--parent-pid");
    DWORD expected_parent_pid = 0;
    if (!parent_pid_str.empty()) {
        expected_parent_pid = static_cast<DWORD>(std::stoul(parent_pid_str));
    }

    if (!verifyParentPid(expected_parent_pid)) {
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

    LOG_INFO("Entering main loop...");

    int heartbeat_counter = 0;
    constexpr int HEARTBEAT_INTERVAL = 100;

    while (true) {
        if (launcher_pipe.isBroken()) {
            LOG_INFO("Launcher pipe broken, monitor shutting down");
            break;
        }

        DWORD launcher_bytes = 0;
        PipeResult launcher_peek = launcher_pipe.peekAvailable(launcher_bytes);

        if (launcher_peek == PipeResult::PIPE_BROKEN) {
            LOG_INFO("Launcher pipe broken (peek), monitor shutting down");
            break;
        }

        if (launcher_peek == PipeResult::PIPE_OK && launcher_bytes > 0) {
            std::string message;
            PipeResult read_result = launcher_pipe.readLine(message, 3000);

            if (read_result == PipeResult::PIPE_OK) {
                LOG_INFO("From launcher: " + message);

                std::string type, cmd, payload;
                if (parseBaseMessage(message, type, cmd, payload)) {
                    if (type == msg_types::INIT_LIST) {
                        handleInitList(payload, launcher_pipe);
                    } else if (type == msg_types::REQUEST_ENGINE) {
                        LOG_WARN("REQUEST_ENGINE not yet implemented");
                    } else if (type == msg_types::MONITOR_GET_ACTIVE_SESSIONS) {
                        std::lock_guard<std::mutex> lock(sessions_mutex_);
                        std::string response;
                        response += R"({"type":"response","cmd":"ACTIVE_SESSIONS_RESP","payload":{"sessions":[)";
                        bool first = true;
                        for (const auto& pair : sessions_) {
                            if (pair.second.state == SessionState::RUNNING) {
                                if (!first) response += ',';
                                first = false;
                                response += R"({"session_id":")" + pair.second.session_id + R"(","pid":)" +
                                            std::to_string(pair.second.process_pid) + "}";
                            }
                        }
                        response += "]}}";
                        (void)launcher_pipe.writeLine(response);
                        LOG_INFO("ACTIVE_SESSIONS_RESP sent");
                    }
                } else {
                    LOG_WARN("Failed to parse base message");
                }

            } else if (read_result == PipeResult::PIPE_BROKEN) {
                LOG_INFO("Launcher pipe broken (read), monitor shutting down");
                break;
            } else if (read_result == PipeResult::PIPE_TIMEOUT) {
                LOG_WARN("Read timeout, will retry");
            }
        }

        // 轮询所有 core_engine 管道
        {
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

        ++heartbeat_counter;
        if (heartbeat_counter % HEARTBEAT_INTERVAL == 0) {
            LOG_INFO("Monitor main loop heartbeat: " + std::to_string(heartbeat_counter) +
                     " iterations, active sessions: " + std::to_string(sessions_.size()));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    LOG_INFO("Shutting down monitor, cleaning up all sessions...");

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

    LOG_INFO("=== Monitor exited ===");
    return 0;
}