// src/core_engine/main.cpp
#include "logger.h"
#include "pipe.h"
#include "constants.h"
#include "messages.h"
#include "event_loop.h"
#include "common_utils.h"

#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <tlhelp32.h>
#include <atomic>

using namespace dream_machine;
using namespace dream_machine::event;
using namespace dream_machine::common;

namespace {

NamedPipe* g_executor_pipe = nullptr;
NamedPipe* g_monitor_pipe = nullptr;
EventLoop* g_event_loop = nullptr;
std::atomic<bool> g_should_stop{false};
std::string g_session_id;

// ----- 处理 monitor 消息（回调） -----
void processMonitorMessage(EventType type, void* user_data) {
    (void)type;
    if (!g_monitor_pipe || g_should_stop) {
        return;
    }

    NamedPipe& monitor_pipe = *g_monitor_pipe;

    if (monitor_pipe.isBroken()) {
        LOG_INFO("Monitor pipe broken, stopping event loop");
        if (g_event_loop) {
            g_event_loop->stop();
        }
        return;
    }

    DWORD bytes_available = 0;
    PipeResult peek_result = monitor_pipe.peekAvailable(bytes_available);

    if (peek_result == PipeResult::PIPE_BROKEN) {
        LOG_INFO("Monitor pipe broken (peek), stopping event loop");
        if (g_event_loop) {
            g_event_loop->stop();
        }
        return;
    }

    if (peek_result != PipeResult::PIPE_OK || bytes_available == 0) {
        return;
    }

    std::string message;
    PipeResult read_result = monitor_pipe.readLineBuffered(message, 100);

    if (read_result == PipeResult::PIPE_OK) {
        LOG_INFO("From monitor: " + message);

        std::string type_str, cmd, payload;
        if (parseBaseMessage(message, type_str, cmd, payload)) {
            if (type_str == msg_types::SHUTDOWN) {
                auto shutdown_msg = parseShutdown(payload);
                if (shutdown_msg.has_value()) {
                    LOG_INFO("Received SHUTDOWN from monitor, exiting");
                    g_should_stop = true;
                    if (g_event_loop) {
                        g_event_loop->stop();
                    }
                }
            }
        } else {
            LOG_WARN("Failed to parse base message from monitor");
        }

    } else if (read_result == PipeResult::PIPE_BROKEN) {
        LOG_INFO("Monitor pipe broken (read), stopping event loop");
        if (g_event_loop) {
            g_event_loop->stop();
        }
    } else if (read_result == PipeResult::PIPE_TIMEOUT) {
        LOG_WARN("Read timeout on monitor pipe, will retry");
    }
}

// ----- 处理 executor 消息（回调） -----
void processExecutorMessage(EventType type, void* user_data) {
    (void)type;
    if (!g_executor_pipe || g_should_stop) {
        return;
    }

    NamedPipe& executor_pipe = *g_executor_pipe;

    if (executor_pipe.isBroken()) {
        LOG_INFO("Executor pipe broken, stopping event loop");
        if (g_event_loop) {
            g_event_loop->stop();
        }
        return;
    }

    DWORD bytes_available = 0;
    PipeResult peek_result = executor_pipe.peekAvailable(bytes_available);

    if (peek_result == PipeResult::PIPE_BROKEN) {
        LOG_INFO("Executor pipe broken (peek), stopping event loop");
        if (g_event_loop) {
            g_event_loop->stop();
        }
        return;
    }

    if (peek_result != PipeResult::PIPE_OK || bytes_available == 0) {
        return;
    }

    std::string message;
    PipeResult read_result = executor_pipe.readLineBuffered(message, 100);

    if (read_result == PipeResult::PIPE_OK) {
        LOG_INFO("From executor: " + message);
        // TODO: 处理操作结果（STEP_*, OP_DONE, OP_ABORT）

    } else if (read_result == PipeResult::PIPE_BROKEN) {
        LOG_INFO("Executor pipe broken (read), stopping event loop");
        if (g_event_loop) {
            g_event_loop->stop();
        }
    } else if (read_result == PipeResult::PIPE_TIMEOUT) {
        LOG_WARN("Read timeout on executor pipe, will retry");
    }
}

// ----- 心跳日志（定时回调） -----
int g_heartbeat_counter = 0;

void logHeartbeat(EventType type, void* user_data) {
    (void)type;
    (void)user_data;

    ++g_heartbeat_counter;
    if (g_heartbeat_counter % 100 == 0) {
        LOG_INFO("Core engine heartbeat: " + std::to_string(g_heartbeat_counter) +
                 " iterations (session: " + g_session_id + ")");
    }
}

} // namespace

// ================================================================
// main 入口
// ================================================================
int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("core_engine");
    LOG_INFO("=== Dream Machine Core Engine starting ===");

    // 使用 common_utils 解析参数并验证父进程
    std::string parent_pid_str = common::getArgValue(argc, argv, "--parent-pid");
    DWORD expected_parent_pid = 0;
    if (!parent_pid_str.empty()) {
        expected_parent_pid = static_cast<DWORD>(std::stoul(parent_pid_str));
    }

    if (!common::verifyParentPid(expected_parent_pid)) {
        return 1;
    }

    g_session_id = common::getArgValue(argc, argv, "--session-id");
    if (g_session_id.empty()) {
        LOG_ERROR("Missing --session-id argument, refusing to run");
        return 1;
    }

    LOG_INFO("Session ID: " + g_session_id);

    // ----- 连接到 executor -----
    std::string executor_pipe_name_str = pipe_names::executor_core();
    std::wstring executor_pipe_name(executor_pipe_name_str.begin(),
                                     executor_pipe_name_str.end());

    LOG_INFO("Connecting to executor pipe: " + executor_pipe_name_str);

    NamedPipe executor_pipe;
    if (!executor_pipe.connect(executor_pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to executor pipe, exiting");
        return 1;
    }

    LOG_INFO("Connected to executor pipe");

    // ----- 连接到 monitor -----
    std::string monitor_pipe_name_str = pipe_names::monitor_core(g_session_id);
    std::wstring monitor_pipe_name(monitor_pipe_name_str.begin(),
                                    monitor_pipe_name_str.end());

    LOG_INFO("Connecting to monitor pipe: " + monitor_pipe_name_str);

    NamedPipe monitor_pipe;
    if (!monitor_pipe.connect(monitor_pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to monitor pipe, exiting");
        return 1;
    }

    LOG_INFO("Connected to monitor pipe");

    // ----- 发送 REGISTER_SESSION -----
    RegisterSessionMessage reg_msg;
    reg_msg.session_id = g_session_id;
    std::string register_msg = serializeRegisterSession(reg_msg);

    if (monitor_pipe.writeLine(register_msg) != PipeResult::PIPE_OK) {
        LOG_ERROR("Failed to send REGISTER_SESSION to monitor, exiting");
        return 1;
    }
    LOG_INFO("REGISTER_SESSION sent to monitor: " + register_msg);

    // ============================================================
    // 初始化事件循环
    // ============================================================
    g_executor_pipe = &executor_pipe;
    g_monitor_pipe = &monitor_pipe;

    EventLoop event_loop;
    g_event_loop = &event_loop;

    EventHandle monitor_read_handle = event_loop.registerReadable(
        monitor_pipe.getHandle(),
        processMonitorMessage,
        nullptr
    );
    if (!monitor_read_handle.active) {
        LOG_ERROR("Failed to register monitor pipe readable event");
        return 1;
    }

    EventHandle executor_read_handle = event_loop.registerReadable(
        executor_pipe.getHandle(),
        processExecutorMessage,
        nullptr
    );
    if (!executor_read_handle.active) {
        LOG_ERROR("Failed to register executor pipe readable event");
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
    // 发送 UNREGISTER_SESSION
    // ============================================================
    LOG_INFO("Sending UNREGISTER_SESSION to monitor...");
    UnregisterSessionMessage unreg_msg;
    unreg_msg.session_id = g_session_id;
    std::string unregister_msg = serializeUnregisterSession(unreg_msg);
    (void)monitor_pipe.writeLine(unregister_msg);

    // ============================================================
    // 清理
    // ============================================================
    LOG_INFO("Shutting down core_engine...");

    event_loop.unregister(monitor_read_handle);
    event_loop.unregister(executor_read_handle);
    event_loop.unregister(heartbeat_handle);
    event_loop.unregister(stop_signal);

    monitor_pipe.close();
    executor_pipe.close();

    g_executor_pipe = nullptr;
    g_monitor_pipe = nullptr;
    g_event_loop = nullptr;

    LOG_INFO("=== Core Engine exited (session: " + g_session_id + ") ===");
    return 0;
}