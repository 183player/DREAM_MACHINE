// platform/dm_base/messages.h
#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

// 前向声明（JSON 序列化使用 QJsonObject，但头文件避免过多依赖）
// 实现在 messages.cpp 中

namespace dream_machine {

// ================================================================
// 消息类型常量（字符串字面量）
// ================================================================
namespace msg_types {

    // ---- 注册/心跳 ----
    inline constexpr const char* REGISTER = "register";
    inline constexpr const char* HEARTBEAT = "heartbeat";

    // ---- 会话管理 ----
    inline constexpr const char* REQUEST_ENGINE = "REQUEST_ENGINE";
    inline constexpr const char* ENGINE_ASSIGNED = "ENGINE_ASSIGNED";
    inline constexpr const char* ENGINE_FAILED = "ENGINE_FAILED";
    inline constexpr const char* SESSION_STATE_CHANGED = "SESSION_STATE_CHANGED";
    inline constexpr const char* SESSION_TERMINATED = "SESSION_TERMINATED";
    inline constexpr const char* MONITOR_GET_ACTIVE_SESSIONS = "MONITOR_GET_ACTIVE_SESSIONS";
    inline constexpr const char* ACTIVE_SESSIONS_RESP = "ACTIVE_SESSIONS_RESP";

    // ---- 插件系统 ----
    inline constexpr const char* INIT_LIST = "INIT_LIST";
    inline constexpr const char* INIT_LIST_ACK = "INIT_LIST_ACK";
    inline constexpr const char* PLUGIN_IMPORT = "PLUGIN_IMPORT";
    inline constexpr const char* PLUGIN_DELETE = "PLUGIN_DELETE";
    inline constexpr const char* PLUGIN_ENABLE = "PLUGIN_ENABLE";
    inline constexpr const char* PLUGIN_IMPORT_RESP = "PLUGIN_IMPORT_RESP";
    inline constexpr const char* PLUGIN_DELETE_RESP = "PLUGIN_DELETE_RESP";
    inline constexpr const char* PLUGIN_ENABLE_RESP = "PLUGIN_ENABLE_RESP";

    // ---- 会话状态更新（gui 专用） ----
    inline constexpr const char* SESSION_STATE_UPDATE = "SESSION_STATE_UPDATE";
    inline constexpr const char* INIT_SESSION_LIST = "INIT_SESSION_LIST";

    // ---- 错误通知 ----
    inline constexpr const char* ERROR_NOTIFY = "ERROR_NOTIFY";
    inline constexpr const char* ENGINE_DIED = "ENGINE_DIED";

    // ---- 进程控制 ----
    inline constexpr const char* SHUTDOWN = "SHUTDOWN";

    // ---- 工具调用（core_engine ↔ executor） ----
    inline constexpr const char* TOOL_CALL = "TOOL_CALL";
    inline constexpr const char* STEP_START = "STEP_START";
    inline constexpr const char* STEP_OK = "STEP_OK";
    inline constexpr const char* STEP_ERR = "STEP_ERR";
    inline constexpr const char* OP_DONE = "OP_DONE";
    inline constexpr const char* OP_ABORT = "OP_ABORT";

    // ---- 脚本执行 ----
    inline constexpr const char* RUN_SCRIPT = "RUN_SCRIPT";
    inline constexpr const char* SCRIPT_RESULT = "SCRIPT_RESULT";

    // ---- 命令类型（用于区分） ----
    inline constexpr const char* CMD = "cmd";
    inline constexpr const char* TYPE = "type";
    inline constexpr const char* PAYLOAD = "payload";

} // namespace msg_types

// ================================================================
// 消息结构体（用于类型安全的序列化/反序列化）
// ================================================================

// ---- 基础消息 ----
struct BaseMessage {
    std::string type;      // msg_types 中的常量
    std::string cmd;       // 子命令（可选）
    std::string payload;   // JSON 字符串（或直接存储对象，但保持灵活性）
};

// ---- 注册消息 ----
struct RegisterMessage {
    std::string process;   // "monitor", "executor", "gui", "core_engine"
    std::optional<std::string> session_id;  // core_engine 注册时提供
};

// ---- 会话相关 ----
struct RequestEngineMessage {
    std::string session_id;
};

struct EngineAssignedMessage {
    std::string session_id;
    std::string pipe_name;
};

struct EngineFailedMessage {
    std::string session_id;
    std::string reason;    // "already_exists", "pipe_create_failed", "launch_failed", "connection_timeout"
};

struct SessionStateChangedMessage {
    std::string session_id;
    std::string state;     // "running", "terminated", "crashed"
    std::optional<std::string> pipe_name;
};

struct SessionStateUpdateMessage {
    std::string session_id;
    std::string state;
};

// ---- 插件相关 ----
struct InitListMessage {
    std::string list_json; // 完整初始化列表 JSON 字符串
};

struct InitListAckMessage {
    std::string status;    // "ok" 或 "error"
    std::optional<std::string> error;
};

struct PluginImportMessage {
    std::string package_path;
};

struct PluginImportRespMessage {
    bool success;
    std::optional<std::string> plugin_id;
    std::optional<std::string> error;
};

struct PluginDeleteMessage {
    std::string plugin_id;
};

struct PluginDeleteRespMessage {
    bool success;
    std::optional<std::string> error;
};

struct PluginEnableMessage {
    std::string plugin_id;
    bool enabled;
};

struct PluginEnableRespMessage {
    bool success;
    std::optional<std::string> error;
};

// ---- 工具调用（core_engine → executor） ----
struct ToolCallMessage {
    std::string tool;      // "read_file", "write_file", "delete_file", "list_dir", "atomic_write_l2"
    std::string params;    // JSON 参数
    std::optional<std::string> session_id;
};

struct StepMessage {
    std::string cmd;       // "STEP_START", "STEP_OK", "STEP_ERR"
    std::string description;
    std::optional<std::string> error;  // 仅 STEP_ERR 时有值
};

struct OpResultMessage {
    std::string cmd;       // "OP_DONE" 或 "OP_ABORT"
    std::optional<std::string> result; // 操作结果 JSON
    std::optional<std::string> error;  // 仅 OP_ABORT 时有值
};

// ---- 脚本执行 ----
struct RunScriptMessage {
    std::string script_path;
    std::string params;    // JSON 参数
    std::optional<std::string> session_id;
};

struct ScriptResultMessage {
    bool success;
    std::optional<std::string> result;  // 脚本返回 JSON
    std::optional<std::string> error;
};

// ---- 错误通知 ----
struct ErrorNotifyMessage {
    std::string source;    // 进程名
    std::string severity;  // "error", "warning", "fatal"
    std::string message;
    std::optional<std::string> details;
};

// ================================================================
// 序列化/反序列化辅助函数（声明）
// ================================================================

// 将各种消息结构体转为 JSON 字符串
std::string serializeRegister(const RegisterMessage& msg);
std::string serializeRequestEngine(const RequestEngineMessage& msg);
std::string serializeEngineAssigned(const EngineAssignedMessage& msg);
std::string serializeEngineFailed(const EngineFailedMessage& msg);
std::string serializeSessionStateChanged(const SessionStateChangedMessage& msg);
std::string serializeSessionStateUpdate(const SessionStateUpdateMessage& msg);
std::string serializeInitList(const InitListMessage& msg);
std::string serializeInitListAck(const InitListAckMessage& msg);
std::string serializePluginImport(const PluginImportMessage& msg);
std::string serializePluginImportResp(const PluginImportRespMessage& msg);
std::string serializePluginDelete(const PluginDeleteMessage& msg);
std::string serializePluginDeleteResp(const PluginDeleteRespMessage& msg);
std::string serializePluginEnable(const PluginEnableMessage& msg);
std::string serializePluginEnableResp(const PluginEnableRespMessage& msg);
std::string serializeToolCall(const ToolCallMessage& msg);
std::string serializeStep(const StepMessage& msg);
std::string serializeOpResult(const OpResultMessage& msg);
std::string serializeRunScript(const RunScriptMessage& msg);
std::string serializeScriptResult(const ScriptResultMessage& msg);
std::string serializeErrorNotify(const ErrorNotifyMessage& msg);

// 从 JSON 字符串解析（返回 std::optional，失败则为 nullopt）
std::optional<RegisterMessage> parseRegister(const std::string& json);
std::optional<RequestEngineMessage> parseRequestEngine(const std::string& json);
std::optional<EngineAssignedMessage> parseEngineAssigned(const std::string& json);
std::optional<EngineFailedMessage> parseEngineFailed(const std::string& json);
std::optional<SessionStateChangedMessage> parseSessionStateChanged(const std::string& json);
std::optional<SessionStateUpdateMessage> parseSessionStateUpdate(const std::string& json);
std::optional<InitListMessage> parseInitList(const std::string& json);
std::optional<InitListAckMessage> parseInitListAck(const std::string& json);
std::optional<PluginImportMessage> parsePluginImport(const std::string& json);
std::optional<PluginImportRespMessage> parsePluginImportResp(const std::string& json);
std::optional<PluginDeleteMessage> parsePluginDelete(const std::string& json);
std::optional<PluginDeleteRespMessage> parsePluginDeleteResp(const std::string& json);
std::optional<PluginEnableMessage> parsePluginEnable(const std::string& json);
std::optional<PluginEnableRespMessage> parsePluginEnableResp(const std::string& json);
std::optional<ToolCallMessage> parseToolCall(const std::string& json);
std::optional<StepMessage> parseStep(const std::string& json);
std::optional<OpResultMessage> parseOpResult(const std::string& json);
std::optional<RunScriptMessage> parseRunScript(const std::string& json);
std::optional<ScriptResultMessage> parseScriptResult(const std::string& json);
std::optional<ErrorNotifyMessage> parseErrorNotify(const std::string& json);

// ================================================================
// 辅助函数：构建通用消息
// ================================================================

// 构建一个标准消息（包含 type, cmd, payload）
std::string buildMessage(const std::string& type,
                         const std::string& cmd,
                         const std::string& payload_json);

// 从标准消息中提取 type, cmd, payload
bool parseBaseMessage(const std::string& json,
                      std::string& out_type,
                      std::string& out_cmd,
                      std::string& out_payload);

} // namespace dream_machine