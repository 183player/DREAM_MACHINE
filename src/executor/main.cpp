// src/executor/main.cpp
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
#include <vector>
#include <atomic>

#include "plugin_types.h"

using namespace dream_machine;
using namespace dream_machine::event;
using namespace dream_machine::common;

namespace {

NamedPipe* g_pipe = nullptr;
EventLoop* g_event_loop = nullptr;
std::atomic<bool> g_should_stop{false};

// 存储从 INIT_LIST 中提取的脚本路径（用于后续执行）
std::vector<std::string> g_script_paths;

// 处理 INIT_LIST 消息
void handleInitList(const std::string& payload) {
    LOG_INFO("Processing INIT_LIST...");

    auto list = plugin::initListFromJson(payload);
    if (!list.has_value()) {
        LOG_ERROR("Failed to parse INIT_LIST payload");
        return;
    }

    g_script_paths.clear();

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

    LOG_INFO("INIT_LIST processing complete, stored " + std::to_string(g_script_paths.size()) + " script paths");
}

// 处理 launcher 消息（事件驱动回调）
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
            if (type_str == msg_types::INIT_LIST) {
                handleInitList(payload);
                InitListAckMessage ack;
                ack.status = "ok";
                std::string ack_json = serializeInitListAck(ack);
                pipe.writeLine(ack_json);
                LOG_INFO("Sent INIT_LIST_ACK");
            } else if (type_str == msg_types::RUN_SCRIPT) {
                LOG_WARN("RUN_SCRIPT not yet implemented");
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
// ================================================================
int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("executor");
    LOG_INFO("=== Dream Machine Executor starting ===");

    // 使用 common_utils 解析参数并验证父进程
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
    std::wstring pipe_name(pipe_name_str.begin(), pipe_name_str.end());

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

    // ============================================================
    // 初始化事件循环
    // ============================================================
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

    // ============================================================
    // 清理
    // ============================================================
    LOG_INFO("Shutting down executor...");

    event_loop.unregister(read_handle);
    event_loop.unregister(heartbeat_handle);
    event_loop.unregister(stop_signal);

    pipe.close();

    g_pipe = nullptr;
    g_event_loop = nullptr;

    LOG_INFO("=== Executor exited ===");
    return 0;
}