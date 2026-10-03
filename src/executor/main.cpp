// src/executor/main.cpp
#include "logger.h"
#include "pipe.h"
#include "constants.h"
#include "messages.h"
#include "message_router.h"
#include "init_list_utils.h"
#include "error_codes.h"
#include "error_notify_bridge.h"   // A.11.2b-2：ErrorContext → ErrorNotifyMessage 映射
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
#include <vector>
#include <atomic>

#include "plugin_types.h"

using namespace dream_machine;
using namespace dream_machine::event;

namespace {

// ================================================================
// ExecutorState：executor 全局状态收敛载体（A.3.2 C3）
// ================================================================
struct ExecutorState {
    // ---- 通信资源 ----
    NamedPipe* pipe = nullptr;   // launcher 管道

    // ---- 生命周期 ----
    EventLoop* event_loop = nullptr;
    std::atomic<bool> should_stop{false};

    // ---- 业务数据 ----
    // 存储从 INIT_LIST 中提取的脚本路径（用于后续 A2 执行）
    std::vector<std::string> script_paths;

    // ---- 消息分发表 ----
    MessageRouter message_router;

    // ---- 心跳 ----
    int heartbeat_counter = 0;
};

static ExecutorState g_state;

void attachSignalBus();
void detachSignalBus();

// ================================================================
// publish_error：通过 SignalBus 发布结构化错误（A.11.2b-2 B3 接线）
//
// 单一映射路径（D5 §8.2 / D3 DREAM-010）：
//   ErrorContext 语义 → SignalPayload → SignalBus
//     ├→ Logger（进程内日志，[signal:error_notify] 前缀）
//     └→ MessageBridge → Sender 重建 ErrorNotifyMessage → 跨进程
//
// 说明：
//   - severity 走 error_severity_to_signal_level_int（受控映射）
//   - error_code 走 errorCodeToString（受控字符串）
//   - extra["error_code"] 携带错误码，供 Sender 重建 details
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
// dm_signal 装配（A.8 装配 + A.11.2b-2 Sender 完整实现）
//
// 订阅顺序（executor）：Logger → OperationTracker → MessageBridge
// 依据：详见文档13 §3.3、文档5 §7.9
//
// 注：
//   - executor 需要 OperationTracker（服务多会话，追踪操作生命周期）
//   - Sender target 当前为 "launcher"（executor 仅有 launcher 管道）
//     * A2 实现后，executor 将作为 core_engine ↔ executor 直连管道的
//       服务端；届时 Sender 应改为 "core_engine" 并新增专用通道
//   - Sender filter 精简为只 SGT_ERROR_NOTIFY（单一映射路径）
// ================================================================
void attachSignalBus() {
    Logger::attach_to_signal_bus();
    signal::SignalBus::instance().subscribe(&signal::OperationTracker::instance());
    signal::SignalBus::instance().subscribe(&signal::MessageBridge::instance());

    // executor → launcher（错误通知；A2 后改为 core_engine）
    (void)signal::MessageBridge::instance().add_sender(
        "launcher",
        [](const signal::SignalPayload& payload) -> bool {
            if (!g_state.pipe || !g_state.pipe->isValid()) {
                return false;
            }

            // A.11.2b-2：从 SignalPayload 重建 ErrorNotifyMessage
            ErrorNotifyMessage notify;
            notify.source = "executor";
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
            PipeResult result = g_state.pipe->writeLine(json);
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

    LOG_INFO("dm_signal attached: Logger + OperationTracker + MessageBridge (executor)");
}

void detachSignalBus() {
    signal::MessageBridge::instance().remove_all_senders();
    Logger::detach_from_signal_bus();
    LOG_INFO("dm_signal detached (executor)");
}

// ================================================================
// 消息 handler 注册
//
// INIT_LIST handler 调用 init_list_utils::processInitList，
// 通过 Hooks 注入 executor 特有行为：
//   - on_parsed    : 清空 g_state.script_paths
//   - on_completed : 输出 "stored N script paths" 日志
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

            init_list_utils::Hooks hooks;
            hooks.on_parsed = []() {
                g_state.script_paths.clear();
            };
            hooks.on_completed = []() {
                LOG_INFO("INIT_LIST processing complete, stored " +
                         std::to_string(g_state.script_paths.size()) + " script paths");
            };

            if (!init_list_utils::processInitList(payload, hooks)) {
                // A.11.2b-2：结构化错误
                publish_error(ErrorCode::JSON_ERROR,
                              ErrorSeverity::WARNING,
                              "Failed to process INIT_LIST",
                              "processInitList returned false");
                return;
            }

            InitListAckMessage ack;
            ack.status = "ok";
            std::string ack_json = serializeInitListAck(ack);
            pipe->writeLine(ack_json);
            LOG_INFO("Sent INIT_LIST_ACK");
        });

    // ---- RUN_SCRIPT ----（A2 未实现，保留占位 + 结构化错误）
    router.register_handler(msg_types::RUN_SCRIPT,
        [](const std::string& /*payload*/, void* /*ctx*/) {
            LOG_WARN("RUN_SCRIPT not yet implemented");
            publish_error(ErrorCode::NOT_IMPLEMENTED,
                          ErrorSeverity::WARNING,
                          "RUN_SCRIPT not yet implemented",
                          "executor A2 pending");
        });
}

// ================================================================
// 处理 launcher 消息（事件驱动回调）
//
// A.11.2b-2：isBroken / 解析失败 → 结构化错误
// ================================================================
void processLauncherMessage(EventType type, void* user_data) {
    (void)type;
    if (!g_state.pipe || g_state.should_stop) {
        return;
    }

    NamedPipe& pipe = *g_state.pipe;

    if (pipe.isBroken()) {
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
    PipeResult peek_result = pipe.peekAvailable(bytes_available);

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
    PipeResult read_result = pipe.readLineBuffered(message, 3000);

    if (read_result == PipeResult::PIPE_OK) {
        LOG_INFO("Received: " + message);

        std::string type_str, cmd, payload;
        if (parseBaseMessage(message, type_str, cmd, payload)) {
            if (!g_state.message_router.dispatch(type_str, payload, &pipe)) {
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

// 心跳日志（定时回调）
void logHeartbeat(EventType type, void* user_data) {
    (void)type;
    (void)user_data;

    ++g_state.heartbeat_counter;
    if (g_state.heartbeat_counter % 100 == 0) {
        LOG_INFO("Executor heartbeat: " + std::to_string(g_state.heartbeat_counter) + " iterations");
    }
}

} // namespace

// ================================================================
// main 入口
//
// 本分片合并改动：
//   A.3.2      C3 全局状态收敛（ExecutorState g_state）
//   A.8        dm_signal 装配（Logger + OperationTracker + MessageBridge）
//   A.11.2b-2  B3 错误结构化接线（Sender 完整实现 + isBroken 结构化错误）
// ================================================================
int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("executor");
    Logger::instance().setLogDirectory(common::pathFromRoot("logs"));

    const bool archived_prev = Logger::instance().archiveLastSessionIfDirty();

    LOG_INFO("=== Dream Machine Executor starting ===");

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
        publish_error(ErrorCode::PROCESS_LAUNCH_FAILED,
                      ErrorSeverity::FATAL,
                      "Parent PID verification failed",
                      "verifyParentPid returned false");
        detachSignalBus();
        return 1;
    }

    std::string pipe_name_str = pipe_names::launcher_executor();
    std::wstring pipe_name = common::utf8ToWide(pipe_name_str);

    LOG_INFO("Connecting to launcher pipe: " + pipe_name_str);

    NamedPipe pipe;
    if (!pipe.connect(pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to launcher pipe, exiting");
        publish_error(ErrorCode::PIPE_CONNECT_FAILED,
                      ErrorSeverity::FATAL,
                      "Failed to connect to launcher pipe",
                      pipe_name_str);
        detachSignalBus();
        return 1;
    }

    LOG_INFO("Connected to launcher pipe");

    RegisterMessage reg_msg;
    reg_msg.process = "executor";
    std::string register_msg = serializeRegister(reg_msg);

    if (pipe.writeLine(register_msg) != PipeResult::PIPE_OK) {
        LOG_ERROR("Failed to send registration message, exiting");
        publish_error(ErrorCode::PIPE_ERROR,
                      ErrorSeverity::FATAL,
                      "Failed to send registration message to launcher",
                      "writeLine returned non-PIPE_OK");
        detachSignalBus();
        return 1;
    }
    LOG_INFO("Registration message sent: " + register_msg);

    g_state.pipe = &pipe;

    EventLoop event_loop;
    g_state.event_loop = &event_loop;

    EventHandle read_handle = event_loop.registerReadable(
        pipe.getHandle(), processLauncherMessage, nullptr);
    if (!read_handle.active) {
        LOG_ERROR("Failed to register launcher pipe readable event");
        publish_error(ErrorCode::PIPE_ERROR,
                      ErrorSeverity::FATAL,
                      "Failed to register launcher pipe readable event",
                      "registerReadable returned inactive handle");
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

    LOG_INFO("Shutting down executor...");

    event_loop.unregister(read_handle);
    event_loop.unregister(heartbeat_handle);
    event_loop.unregister(stop_signal);

    pipe.close();
    g_state.pipe = nullptr;
    g_state.event_loop = nullptr;

    // A.8：dm_signal 卸载
    detachSignalBus();

    LOG_INFO("=== Executor exited ===");

    Logger::instance().markCleanExit();

    return 0;
}