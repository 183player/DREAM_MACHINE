// src/core_engine/main.cpp
#include "logger.h"
#include "pipe.h"
#include "constants.h"
#include "messages.h"
#include "message_router.h"
#include "event_loop.h"
#include "common_utils.h"

// dm_signal（A.8 装配）
#include "signal_bus.h"
#include "message_bridge.h"
#include "signal_types.h"
#include "signal_strings.h"
#include "operation_tracker.h"

#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <tlhelp32.h>
#include <atomic>

using namespace dream_machine;
using namespace dream_machine::event;

namespace {

// ================================================================
// CoreEngineState：core_engine 全局状态收敛载体（A.3.2 C3）
//
// 未来重审点（依据 DREAM_MACHINE_CONCURRENCY_MODEL_BOUNDARY 专家裁决 Q4）：
//   1. 若 core_engine 引入业务内聚线程（如四层漏斗的路由层异步解析），需重审：
//        - executor_pipe / monitor_pipe 改为 std::shared_ptr<NamedPipe>
//        - session_id 改为线程安全访问
//      注：执行路径（executor 调用）即使多线程也难以调试，搁置优先。
//   2. 日志生命周期：
//        启动时调用 archiveLastSessionIfDirty() 检测上次异常退出；
//        正常退出前调用 markCleanExit() 写入标记。
//        本文件已调整 setProcessName 顺序以配合多实例日志命名。
// ================================================================
struct CoreEngineState {
    // ---- 通信资源 ----
    NamedPipe* executor_pipe = nullptr;
    NamedPipe* monitor_pipe = nullptr;

    // ---- 生命周期 ----
    EventLoop* event_loop = nullptr;
    std::atomic<bool> should_stop{false};

    // ---- 会话 ----
    std::string session_id;

    // ---- 消息分发表 ----
    MessageRouter monitor_router;

    // ---- 心跳 ----
    int heartbeat_counter = 0;
};

static CoreEngineState g_state;

// ----- 前向声明 -----
void registerMonitorMessageHandlers(MessageRouter& router);
void attachSignalBus();
void detachSignalBus();

// ================================================================
// dm_signal 装配（A.8）
//
// 订阅顺序（core_engine）：Logger → OperationTracker → MessageBridge
// 依据：详见文档13 §3.3、文档5 §7.9
//
// 注：
//   - core_engine 需要 OperationTracker（单会话，统一装配便于一致性）
//   - Sender 捕获 g_state（static，安全）
//   - 当前 Sender 占位，待 A.11 完整接入
// ================================================================
void attachSignalBus() {
    Logger::attach_to_signal_bus();
    signal::SignalBus::instance().subscribe(&signal::OperationTracker::instance());
    signal::SignalBus::instance().subscribe(&signal::MessageBridge::instance());

    // core_engine → monitor（操作上下文 / 错误通知）
    (void)signal::MessageBridge::instance().add_sender(
        "monitor",
        [](const signal::SignalPayload& payload) -> bool {
            if (!g_state.monitor_pipe || !g_state.monitor_pipe->isValid()) {
                return false;
            }
            // TODO(A.11)：序列化 SignalPayload → ErrorNotifyMessage / OP_CONTEXT
            LOG_INFO(std::string("[MessageBridge] signal -> monitor: type=") +
                     signal::signal_type_to_string(payload.type) +
                     " | " + payload.description);
            return true;
        },
        [](const signal::SignalPayload& payload) -> bool {
            return payload.type == signal::SignalType::SGT_OP_CONTEXT ||
                   payload.type == signal::SignalType::SGT_ERROR_NOTIFY ||
                   payload.type == signal::SignalType::SGT_ENGINE_DIED;
        });

    LOG_INFO("dm_signal attached: Logger + OperationTracker + MessageBridge (core_engine)");
}

void detachSignalBus() {
    signal::MessageBridge::instance().remove_all_senders();
    Logger::detach_from_signal_bus();
    LOG_INFO("dm_signal detached (core_engine)");
}

// ================================================================
// monitor 消息 handler 注册
// ================================================================
void registerMonitorMessageHandlers(MessageRouter& router) {
    // ---- SHUTDOWN ----
    router.register_handler(msg_types::SHUTDOWN,
        [](const std::string& payload, void* /*ctx*/) {
            auto shutdown_msg = parseShutdown(payload);
            std::string reason = shutdown_msg.has_value()
                                 ? shutdown_msg->reason
                                 : std::string(shutdown_reason::PEER_EXIT);

            LOG_INFO("Received SHUTDOWN from monitor, reason=" + reason);

            g_state.should_stop = true;
            if (g_state.event_loop) {
                g_state.event_loop->stop();
            }
        });
}

// ================================================================
// 处理 monitor 消息（回调）
// ================================================================
void processMonitorMessage(EventType type, void* user_data) {
    (void)type;
    if (!g_state.monitor_pipe || g_state.should_stop) {
        return;
    }

    NamedPipe& monitor_pipe = *g_state.monitor_pipe;

    if (monitor_pipe.isBroken()) {
        LOG_INFO("Monitor pipe broken, stopping event loop");
        if (g_state.event_loop) {
            g_state.event_loop->stop();
        }
        return;
    }

    DWORD bytes_available = 0;
    PipeResult peek_result = monitor_pipe.peekAvailable(bytes_available);

    if (peek_result == PipeResult::PIPE_BROKEN) {
        LOG_INFO("Monitor pipe broken (peek), stopping event loop");
        if (g_state.event_loop) {
            g_state.event_loop->stop();
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
            if (!g_state.monitor_router.dispatch(type_str, payload, &monitor_pipe)) {
                LOG_WARN("Unhandled message type from monitor: " + type_str);
            }
        } else {
            LOG_WARN("Failed to parse base message from monitor");
        }

    } else if (read_result == PipeResult::PIPE_BROKEN) {
        LOG_INFO("Monitor pipe broken (read), stopping event loop");
        if (g_state.event_loop) {
            g_state.event_loop->stop();
        }
    } else if (read_result == PipeResult::PIPE_TIMEOUT) {
        LOG_WARN("Read timeout on monitor pipe, will retry");
    }
}

// ================================================================
// 处理 executor 消息（回调）
//
// 注：executor 侧消息处理尚未实现（A3），保持 TODO 原样。
// ================================================================
void processExecutorMessage(EventType type, void* user_data) {
    (void)type;
    if (!g_state.executor_pipe || g_state.should_stop) {
        return;
    }

    NamedPipe& executor_pipe = *g_state.executor_pipe;

    if (executor_pipe.isBroken()) {
        LOG_INFO("Executor pipe broken, stopping event loop");
        if (g_state.event_loop) {
            g_state.event_loop->stop();
        }
        return;
    }

    DWORD bytes_available = 0;
    PipeResult peek_result = executor_pipe.peekAvailable(bytes_available);

    if (peek_result == PipeResult::PIPE_BROKEN) {
        LOG_INFO("Executor pipe broken (peek), stopping event loop");
        if (g_state.event_loop) {
            g_state.event_loop->stop();
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
        // TODO(A3): 处理操作结果（STEP_*, OP_DONE, OP_ABORT）

    } else if (read_result == PipeResult::PIPE_BROKEN) {
        LOG_INFO("Executor pipe broken (read), stopping event loop");
        if (g_state.event_loop) {
            g_state.event_loop->stop();
        }
    } else if (read_result == PipeResult::PIPE_TIMEOUT) {
        LOG_WARN("Read timeout on executor pipe, will retry");
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
        LOG_INFO("Core engine heartbeat: " + std::to_string(g_state.heartbeat_counter) +
                 " iterations (session: " + g_state.session_id + ")");
    }
}

} // namespace

// ================================================================
// main 入口
//
// 路径策略：所有运行时资源基于 exe 目录，不依赖 CWD。
//
// 日志生命周期：
//   - 启动：解析参数 → setProcessName → setLogDirectory
//           → archiveLastSessionIfDirty → 开始日志
//   - 退出：最后一条日志 → markCleanExit → return 0
//
// 失败路径（父进程校验、session_id 校验、连接、事件注册失败）不写 .clean_exit。
//
// 本分片合并改动：
//   A.3.2  C3 全局状态收敛（CoreEngineState g_state）
//   A.8    dm_signal 装配（Logger + OperationTracker + MessageBridge）
// ================================================================
int main(int argc, char* argv[]) {
    // ----- 1. 先解析命令行参数（无日志） -----
    std::string parent_pid_str = common::getArgValue(argc, argv, "--parent-pid");
    DWORD expected_parent_pid = 0;
    if (!parent_pid_str.empty()) {
        expected_parent_pid = static_cast<DWORD>(std::stoul(parent_pid_str));
    }

    g_state.session_id = common::getArgValue(argc, argv, "--session-id");

    // ----- 2. 设置进程名（依赖 session_id） -----
    if (g_state.session_id.empty()) {
        Logger::instance().setProcessName("core_engine");
    } else {
        Logger::instance().setProcessName("core_engine_" + g_state.session_id);
    }

    // ----- 3. 设置日志目录 -----
    Logger::instance().setLogDirectory(common::pathFromRoot("logs"));

    // ----- 4. 归档检测 -----
    const bool archived_prev = Logger::instance().archiveLastSessionIfDirty();

    // ----- 5. 开始日志输出 -----
    LOG_INFO("=== Dream Machine Core Engine starting ===");

    if (archived_prev) {
        LOG_INFO("Previous session logs archived to logs/crashes/");
    }

    // A.8：dm_signal 装配
    attachSignalBus();

    registerMonitorMessageHandlers(g_state.monitor_router);

    // ----- 6. 校验父进程 -----
    if (!common::verifyParentPid(expected_parent_pid)) {
        detachSignalBus();
        return 1;
    }

    // ----- 7. 校验 session_id -----
    if (g_state.session_id.empty()) {
        LOG_ERROR("Missing --session-id argument, refusing to run");
        detachSignalBus();
        return 1;
    }

    LOG_INFO("Session ID: " + g_state.session_id);

    // ----- 连接到 executor -----
    std::string executor_pipe_name_str = pipe_names::executor_core();
    std::wstring executor_pipe_name = common::utf8ToWide(executor_pipe_name_str);

    LOG_INFO("Connecting to executor pipe: " + executor_pipe_name_str);

    NamedPipe executor_pipe;
    if (!executor_pipe.connect(executor_pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to executor pipe, exiting");
        detachSignalBus();
        return 1;
    }

    LOG_INFO("Connected to executor pipe");

    // ----- 连接到 monitor -----
    std::string monitor_pipe_name_str = pipe_names::monitor_core(g_state.session_id);
    std::wstring monitor_pipe_name = common::utf8ToWide(monitor_pipe_name_str);

    LOG_INFO("Connecting to monitor pipe: " + monitor_pipe_name_str);

    NamedPipe monitor_pipe;
    if (!monitor_pipe.connect(monitor_pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to monitor pipe, exiting");
        detachSignalBus();
        return 1;
    }

    LOG_INFO("Connected to monitor pipe");

    // ----- 发送 REGISTER_SESSION -----
    RegisterSessionMessage reg_msg;
    reg_msg.session_id = g_state.session_id;
    std::string register_msg = serializeRegisterSession(reg_msg);

    if (monitor_pipe.writeLine(register_msg) != PipeResult::PIPE_OK) {
        LOG_ERROR("Failed to send REGISTER_SESSION to monitor, exiting");
        detachSignalBus();
        return 1;
    }
    LOG_INFO("REGISTER_SESSION sent to monitor: " + register_msg);

    // ============================================================
    // 初始化事件循环
    // ============================================================
    g_state.executor_pipe = &executor_pipe;
    g_state.monitor_pipe = &monitor_pipe;

    EventLoop event_loop;
    g_state.event_loop = &event_loop;

    EventHandle monitor_read_handle = event_loop.registerReadable(
        monitor_pipe.getHandle(), processMonitorMessage, nullptr);
    if (!monitor_read_handle.active) {
        LOG_ERROR("Failed to register monitor pipe readable event");
        detachSignalBus();
        return 1;
    }

    EventHandle executor_read_handle = event_loop.registerReadable(
        executor_pipe.getHandle(), processExecutorMessage, nullptr);
    if (!executor_read_handle.active) {
        LOG_ERROR("Failed to register executor pipe readable event");
        detachSignalBus();
        return 1;
    }

    EventHandle heartbeat_handle = event_loop.registerTimer(
        100, logHeartbeat, nullptr, false);
    if (!heartbeat_handle.active) {
        LOG_ERROR("Failed to register heartbeat timer");
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

    // ============================================================
    // 发送 UNREGISTER_SESSION
    // ============================================================
    LOG_INFO("Sending UNREGISTER_SESSION to monitor...");
    UnregisterSessionMessage unreg_msg;
    unreg_msg.session_id = g_state.session_id;
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

    g_state.executor_pipe = nullptr;
    g_state.monitor_pipe = nullptr;
    g_state.event_loop = nullptr;

    // A.8：dm_signal 卸载
    detachSignalBus();

    LOG_INFO("=== Core Engine exited (session: " + g_state.session_id + ") ===");

    Logger::instance().markCleanExit();

    return 0;
}