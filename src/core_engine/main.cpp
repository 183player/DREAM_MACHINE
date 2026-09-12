// src/core_engine/main.cpp
#include "logger.h"
#include "pipe.h"
#include "constants.h"
#include "messages.h"
#include "message_router.h"
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
// 注：阶段 1.7 C4 起不再 using dream_machine::common——
//     新增的 utf8ToWide / wideToUtf8 用 common:: 前缀显式调用。

namespace {

// 全局状态
//
// 未来重审点（依据 DREAM_MACHINE_CONCURRENCY_MODEL_BOUNDARY 专家裁决 Q4）：
//   1. 若 core_engine 引入业务内聚线程（如三层漏斗的路由层异步解析），需重审：
//        - g_executor_pipe / g_monitor_pipe 改为 std::shared_ptr<NamedPipe>
//        - g_session_id 改为线程安全访问
//      注：执行路径（executor 调用）即使多线程也难以调试，搁置优先。
//   2. 日志生命周期（阶段 1.5 P1-5）：
//        启动时调用 archiveLastSessionIfDirty() 检测上次异常退出；
//        正常退出前调用 markCleanExit() 写入标记。
//        本文件已调整 setProcessName 顺序以配合多实例日志命名。
NamedPipe* g_executor_pipe = nullptr;
NamedPipe* g_monitor_pipe = nullptr;
EventLoop* g_event_loop = nullptr;
std::atomic<bool> g_should_stop{false};
std::string g_session_id;

// 消息分发表（阶段 1.7 C1）
//
// monitor 侧：迁移了 1 个 handler（SHUTDOWN）。
// executor 侧：STEP_* / OP_DONE / OP_ABORT 尚未实现，
//              保持 TODO 原样，未来实现 A3 时接入。
//
// 回退方式：删除本变量 + registerMonitorMessageHandlers 调用，
//          并将 processMonitorMessage 恢复为原始 if-else 即可。
MessageRouter g_monitor_router;

// ----- 前向声明 -----
void registerMonitorMessageHandlers(MessageRouter& router);

// ================================================================
// monitor 消息 handler 注册（阶段 1.7 C1）
// ================================================================
void registerMonitorMessageHandlers(MessageRouter& router) {
    // ---- SHUTDOWN ----
    // 依据 DREAM_MACHINE_SHUTDOWN_COORDINATION 裁决：
    //   - 接收方自行决定退出时机，广播方不强制
    //   - 记录 reason 用于日志区分（peer_exit / user_close / signal）
    //   - 即使 payload 解析失败也按默认 reason 处理，避免僵死
    router.register_handler(msg_types::SHUTDOWN,
        [](const std::string& payload, void* /*ctx*/) {
            auto shutdown_msg = parseShutdown(payload);
            std::string reason = shutdown_msg.has_value()
                                 ? shutdown_msg->reason
                                 : std::string(shutdown_reason::PEER_EXIT);

            LOG_INFO("Received SHUTDOWN from monitor, reason=" + reason);

            g_should_stop = true;
            if (g_event_loop) {
                g_event_loop->stop();
            }
        });
}

// ================================================================
// 处理 monitor 消息（回调）
//
// 阶段 1.7 C1：走 MessageRouter 分发。
// ================================================================
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
            // ---- 走分发表（阶段 1.7 C1） ----
            if (!g_monitor_router.dispatch(type_str, payload, &monitor_pipe)) {
                LOG_WARN("Unhandled message type from monitor: " + type_str);
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

// ================================================================
// 处理 executor 消息（回调）
//
// 注：executor 侧消息处理尚未实现（A3 任务），保持 TODO 原样。
//     未来实现时，可参考 monitor 侧接入 MessageRouter。
// ================================================================
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

// ================================================================
// 心跳日志（定时回调）
// ================================================================
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
//
// 顺序调整说明（阶段 1.5 P1-6）：
//   先解析命令行参数（无日志），再根据 session_id 设置进程名，
//   然后才开始日志输出。避免早期日志写入 "core_engine.log" 后
//   切换到 "core_engine_{session}.log" 造成孤立文件。
//
// 日志文件名规则：
//   session_id 有效 → "core_engine_{session_id}.log"
//   session_id 缺失 → "core_engine.log"（仅在拒绝运行前记录诊断信息）
//
// 阶段 1.7 C1：monitor 侧消息分发表接入（1 个 handler 迁移）
// ================================================================
int main(int argc, char* argv[]) {
    // ----- 1. 先解析命令行参数（无日志） -----
    std::string parent_pid_str = common::getArgValue(argc, argv, "--parent-pid");
    DWORD expected_parent_pid = 0;
    if (!parent_pid_str.empty()) {
        expected_parent_pid = static_cast<DWORD>(std::stoul(parent_pid_str));
    }

    g_session_id = common::getArgValue(argc, argv, "--session-id");

    // ----- 2. 设置进程名 -----
    if (g_session_id.empty()) {
        Logger::instance().setProcessName("core_engine");
    } else {
        Logger::instance().setProcessName("core_engine_" + g_session_id);
    }

    // ----- 3. 开始日志输出 -----
    LOG_INFO("=== Dream Machine Core Engine starting ===");

    // 阶段 1.7 C1：注册 monitor 侧消息 handler（必须在事件循环启动前）
    registerMonitorMessageHandlers(g_monitor_router);

    // ----- 4. 校验父进程 -----
    if (!common::verifyParentPid(expected_parent_pid)) {
        return 1;
    }

    // ----- 5. 校验 session_id -----
    if (g_session_id.empty()) {
        LOG_ERROR("Missing --session-id argument, refusing to run");
        return 1;
    }

    LOG_INFO("Session ID: " + g_session_id);

    // ----- 连接到 executor -----
    std::string executor_pipe_name_str = pipe_names::executor_core();

    // C4 编码 helper（阶段 1.7）
    std::wstring executor_pipe_name = common::utf8ToWide(executor_pipe_name_str);

    LOG_INFO("Connecting to executor pipe: " + executor_pipe_name_str);

    NamedPipe executor_pipe;
    if (!executor_pipe.connect(executor_pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to executor pipe, exiting");
        return 1;
    }

    LOG_INFO("Connected to executor pipe");

    // ----- 连接到 monitor -----
    std::string monitor_pipe_name_str = pipe_names::monitor_core(g_session_id);

    // C4 编码 helper（阶段 1.7）：同上
    std::wstring monitor_pipe_name = common::utf8ToWide(monitor_pipe_name_str);

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

    Logger::instance().markCleanExit();

    return 0;
}