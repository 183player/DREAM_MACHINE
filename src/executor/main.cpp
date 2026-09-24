// src/executor/main.cpp
#include "logger.h"
#include "pipe.h"
#include "constants.h"
#include "messages.h"
#include "message_router.h"
#include "init_list_utils.h"
#include "event_loop.h"
#include "common_utils.h"

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
// 注：不再 using dream_machine::common——
//     新增的 utf8ToWide / wideToUtf8 / pathFromRoot 用 common:: 前缀显式调用。

namespace {

// 全局状态
//
// 未来重审点（依据 DREAM_MACHINE_CONCURRENCY_MODEL_BOUNDARY 专家裁决 Q4）：
//   若 executor 未来引入业务内聚线程（如异步脚本执行），需重审：
//     - g_pipe 改为 std::shared_ptr<NamedPipe>
//     - g_script_paths 加锁或改线程安全结构
//   当前单线程事件驱动模型下无需改造。
//   （注：executor 执行路径多线程调试困难，优先搁置）
NamedPipe* g_pipe = nullptr;
EventLoop* g_event_loop = nullptr;
std::atomic<bool> g_should_stop{false};

// 存储从 INIT_LIST 中提取的脚本路径（用于后续执行）
std::vector<std::string> g_script_paths;

// 消息分发表
//
// 迁移了全部 3 个 handler（SHUTDOWN / INIT_LIST / RUN_SCRIPT）。
// 无 fallback 特例——所有消息类型统一走 router.dispatch。
//
// 回退方式：删除本变量 + registerMessageHandlers 调用，
//          并将 processLauncherMessage 恢复为原始 if-else 即可。
MessageRouter g_message_router;

// ================================================================
// 消息 handler 注册
//
// 注册全部 3 个 handler 到全局 router。
//
// INIT_LIST handler 调用 init_list_utils::processInitList，
// 通过 Hooks 注入 executor 特有行为：
//   - on_parsed    : 清空 g_script_paths
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

            g_should_stop = true;
            if (g_event_loop) {
                g_event_loop->stop();
            }
        });

    // ---- INIT_LIST ----
    router.register_handler(msg_types::INIT_LIST,
        [](const std::string& payload, void* ctx) {
            auto* pipe = static_cast<NamedPipe*>(ctx);
            if (!pipe) return;

            init_list_utils::Hooks hooks;
            hooks.on_parsed = []() {
                g_script_paths.clear();
            };
            hooks.on_completed = []() {
                LOG_INFO("INIT_LIST processing complete, stored " +
                         std::to_string(g_script_paths.size()) + " script paths");
            };

            if (!init_list_utils::processInitList(payload, hooks)) {
                return;  // 解析失败，不发 ACK
            }

            InitListAckMessage ack;
            ack.status = "ok";
            std::string ack_json = serializeInitListAck(ack);
            pipe->writeLine(ack_json);
            LOG_INFO("Sent INIT_LIST_ACK");
        });

    // ---- RUN_SCRIPT ----
    router.register_handler(msg_types::RUN_SCRIPT,
        [](const std::string& /*payload*/, void* /*ctx*/) {
            LOG_WARN("RUN_SCRIPT not yet implemented");
        });
}

// ================================================================
// 处理 launcher 消息（事件驱动回调）
// ================================================================
void processLauncherMessage(EventType type, void* user_data) {
    (void)type;
    if (!g_pipe || g_should_stop) {
        return;
    }

    NamedPipe& pipe = *g_pipe;

    if (pipe.isBroken()) {
        LOG_INFO("Launcher pipe broken, stopping event loop");
        if (g_event_loop) {
            g_event_loop->stop();
        }
        return;
    }

    DWORD bytes_available = 0;
    PipeResult peek_result = pipe.peekAvailable(bytes_available);

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
    PipeResult read_result = pipe.readLineBuffered(message, 3000);

    if (read_result == PipeResult::PIPE_OK) {
        LOG_INFO("Received: " + message);

        std::string type_str, cmd, payload;
        if (parseBaseMessage(message, type_str, cmd, payload)) {
            if (!g_message_router.dispatch(type_str, payload, &pipe)) {
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

// 心跳日志（定时回调）
int g_heartbeat_counter = 0;

void logHeartbeat(EventType type, void* user_data) {
    (void)type;
    (void)user_data;

    ++g_heartbeat_counter;
    if (g_heartbeat_counter % 100 == 0) {
        LOG_INFO("Executor heartbeat: " + std::to_string(g_heartbeat_counter) + " iterations");
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
    Logger::instance().setProcessName("executor");

    // 路径修正：日志目录基于 exe 目录，不依赖 CWD
    Logger::instance().setLogDirectory(common::pathFromRoot("logs"));

    const bool archived_prev = Logger::instance().archiveLastSessionIfDirty();

    LOG_INFO("=== Dream Machine Executor starting ===");

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

    // 连接到 launcher
    std::string pipe_name_str = pipe_names::launcher_executor();

    std::wstring pipe_name = common::utf8ToWide(pipe_name_str);

    LOG_INFO("Connecting to launcher pipe: " + pipe_name_str);

    NamedPipe pipe;
    if (!pipe.connect(pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to launcher pipe, exiting");
        return 1;
    }

    LOG_INFO("Connected to launcher pipe");

    // 发送注册消息
    RegisterMessage reg_msg;
    reg_msg.process = "executor";
    std::string register_msg = serializeRegister(reg_msg);

    if (pipe.writeLine(register_msg) != PipeResult::PIPE_OK) {
        LOG_ERROR("Failed to send registration message, exiting");
        return 1;
    }
    LOG_INFO("Registration message sent: " + register_msg);

    g_pipe = &pipe;

    EventLoop event_loop;
    g_event_loop = &event_loop;

    EventHandle read_handle = event_loop.registerReadable(
        pipe.getHandle(),
        processLauncherMessage,
        nullptr
    );
    if (!read_handle.active) {
        LOG_ERROR("Failed to register launcher pipe readable event");
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

    LOG_INFO("Shutting down executor...");

    event_loop.unregister(read_handle);
    event_loop.unregister(heartbeat_handle);
    event_loop.unregister(stop_signal);

    pipe.close();

    g_pipe = nullptr;
    g_event_loop = nullptr;

    LOG_INFO("=== Executor exited ===");

    Logger::instance().markCleanExit();

    return 0;
}