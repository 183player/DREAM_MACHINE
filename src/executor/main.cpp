// src/executor/main.cpp
#include "logger.h"
#include "pipe.h"
#include "constants.h"
#include "messages.h"

#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <tlhelp32.h>
#include <vector>

#include "plugin_types.h"

using namespace dream_machine;

namespace {

std::string getArgValue(int argc, char* argv[], const std::string& key) {
    for (int i = 1; i < argc - 1; ++i) {
        if (argv[i] == key) {
            return argv[i + 1];
        }
    }
    return {};
}

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

// 存储从 INIT_LIST 中提取的脚本路径（用于后续执行）
std::vector<std::string> g_script_paths;

// 处理 INIT_LIST 消息
void handleInitList(const std::string& payload) {
    LOG_INFO("Processing INIT_LIST...");

    // 解析 INIT_LIST 获取脚本路径
    // 由于 payload 是 JSON 字符串，我们使用 plugin::initListFromJson 解析
    // 但 initListFromJson 期望完整的列表 JSON，而 payload 就是列表 JSON
    auto list = plugin::initListFromJson(payload);
    if (!list.has_value()) {
        LOG_ERROR("Failed to parse INIT_LIST payload");
        return;
    }

    // 清空旧的脚本路径
    g_script_paths.clear();

    // 遍历 entries，收集脚本路径（如果有）
    for (const auto& entry : list->entries) {
        // 目前我们只关心 future 扩展，暂时不存储具体内容
        // 例如，如果 entry 有 rule_file 或 trigger，可以保存
        // 当前仅记录日志
        if (entry.type == plugin::ModificationType::REPLACE) {
            LOG_INFO("REPLACE: target=" + entry.target_file + ", winner=" + entry.winner_plugin_id);
        } else if (entry.type == plugin::ModificationType::EXTEND) {
            LOG_INFO("EXTEND: container=" + entry.container_id + ", plugin=" + entry.plugin_id);
        }
    }

    LOG_INFO("INIT_LIST processing complete, stored " + std::to_string(g_script_paths.size()) + " script paths");
}

} // namespace

int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("executor");
    LOG_INFO("=== Dream Machine Executor starting ===");

    // 验证启动凭证
    std::string parent_pid_str = getArgValue(argc, argv, "--parent-pid");
    DWORD expected_parent_pid = 0;
    if (!parent_pid_str.empty()) {
        expected_parent_pid = static_cast<DWORD>(std::stoul(parent_pid_str));
    }

    if (!verifyParentPid(expected_parent_pid)) {
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

    // 主循环
    LOG_INFO("Entering main loop...");

    int heartbeat_counter = 0;
    constexpr int HEARTBEAT_INTERVAL = 100;

    while (true) {
        if (pipe.isBroken()) {
            LOG_INFO("Launcher pipe broken, executor shutting down");
            break;
        }

        DWORD bytes_available = 0;
        PipeResult peek_result = pipe.peekAvailable(bytes_available);

        if (peek_result == PipeResult::PIPE_BROKEN) {
            LOG_INFO("Launcher pipe broken (peek), executor shutting down");
            break;
        }

        if (peek_result == PipeResult::PIPE_OK && bytes_available > 0) {
            std::string message;
            // 使用较长超时确保读取完整消息
            PipeResult read_result = pipe.readLine(message, 3000);

            if (read_result == PipeResult::PIPE_OK) {
                LOG_INFO("Received: " + message);

                std::string type, cmd, payload;
                if (parseBaseMessage(message, type, cmd, payload)) {
                    if (type == msg_types::INIT_LIST) {
                        handleInitList(payload);
                        // 发送 ACK
                        InitListAckMessage ack;
                        ack.status = "ok";
                        std::string ack_json = serializeInitListAck(ack);
                        pipe.writeLine(ack_json);
                        LOG_INFO("Sent INIT_LIST_ACK");
                    }
                    // 其他消息类型（如 RUN_SCRIPT）将在后续实现
                } else {
                    LOG_WARN("Failed to parse base message");
                }

            } else if (read_result == PipeResult::PIPE_BROKEN) {
                LOG_INFO("Launcher pipe broken (read), executor shutting down");
                break;
            } else if (read_result == PipeResult::PIPE_TIMEOUT) {
                LOG_WARN("Read timeout, will retry");
            }
        }

        ++heartbeat_counter;
        if (heartbeat_counter % HEARTBEAT_INTERVAL == 0) {
            LOG_INFO("Executor main loop heartbeat: " + std::to_string(heartbeat_counter) + " iterations");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    LOG_INFO("Shutting down executor...");
    pipe.close();
    LOG_INFO("=== Executor exited ===");
    return 0;
}