// platform/dm_base/messages.h
#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

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

    // ---- 会话注册（core_engine ↔ monitor） ----
    inline constexpr const char* REGISTER_SESSION = "REGISTER_SESSION";
    inline constexpr const char* UNREGISTER_SESSION = "UNREGISTER_SESSION";

    // ---- 全量同步 ----
    inline constexpr const char* FULL_SYNC_REQUEST = "FULL_SYNC_REQUEST";
    inline constexpr const char* FULL_SYNC_RESPONSE = "FULL_SYNC_RESPONSE";

    // ---- 命令类型（用于区分） ----
    inline constexpr const char* CMD = "cmd";
    inline constexpr const char* TYPE = "type";
    inline constexpr const char* PAYLOAD = "payload";
}

// ================================================================
// 协议版本（阶段 1.7 B4）
//
// buildMessage 自动将 version 写入所有消息，parseBaseMessage 读取。
// 当前设计（保守）：
//   - 只提供字段与常量，不做版本协商
//   - 版本不匹配仅记录 WARN，仍继续处理（不拒绝）
//   - 未来若需协商，可在此基础上扩展
// ================================================================
namespace msg_version {
    inline constexpr int CURRENT = 1;
}

// ================================================================
// SHUTDOWN 消息受控常量
//
// 依据 DREAM_MACHINE_SHUTDOWN_COORDINATION 专家裁决：
//   - reason:    "peer_exit" | "user_close" | "signal"
//   - initiator: "launcher" | "monitor"
//
// 使用方式：接收方只记录不解析时可直接记日志；
//          需要分支判断时用常量比较，禁止字符串硬编码。
// ================================================================
namespace shutdown_reason {
    inline constexpr const char* PEER_EXIT  = "peer_exit";   // 级联退出（对端进程退出触发）
    inline constexpr const char* USER_CLOSE = "user_close";  // 用户主动关闭（GUI 关闭按钮）
    inline constexpr const char* SIGNAL     = "signal";      // 外部信号触发
}

namespace shutdown_initiator {
    inline constexpr const char* LAUNCHER = "launcher";  // 由 launcher 发起广播
    inline constexpr const char* MONITOR  = "monitor";   // 由 monitor 下行转发
}

// ================================================================
// 消息结构体
// ================================================================

// ---- 基础消息 ----
// version 由 buildMessage 自动写入，parseBaseMessage 读取。
// 现有调用点无需感知；需要版本感知时可使用新增的 parseBaseMessage 重载。
struct BaseMessage {
    std::string type;
    std::string cmd;
    std::string payload;
    int version = msg_version::CURRENT;
};

// ---- 注册 ----
struct RegisterMessage {
    std::string process;
    std::optional<std::string> session_id;
};

// ---- 会话管理 ----
struct RequestEngineMessage {
    std::string session_id;
};

struct EngineAssignedMessage {
    std::string session_id;
    std::string pipe_name;
};

struct EngineFailedMessage {
    std::string session_id;
    std::string reason;
};

struct SessionStateChangedMessage {
    std::string session_id;
    std::string state;
    std::optional<std::string> pipe_name;
};

struct SessionTerminatedMessage {
    std::string session_id;
    std::string reason;
};

struct SessionStateUpdateMessage {
    std::string session_id;
    std::string state;
};

// ---- 初始化会话列表（新增） ----
struct InitSessionListMessage {
    std::vector<SessionStateUpdateMessage> sessions;  // 当前空列表，保留扩展
};

// ---- 全量同步 ----
struct FullSyncRequestMessage {
    int64_t request_id;
};

struct FullSyncResponseMessage {
    int64_t request_id;
    std::vector<SessionStateChangedMessage> sessions;
};

// ---- 会话注册 ----
struct RegisterSessionMessage {
    std::string session_id;
};

struct UnregisterSessionMessage {
    std::string session_id;
};

// ---- 插件相关 ----
struct InitListMessage {
    std::string list_json;
};

struct InitListAckMessage {
    std::string status;
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

// ---- 工具调用 ----
struct ToolCallMessage {
    std::string tool;
    std::string params;
    std::optional<std::string> session_id;
};

struct StepMessage {
    std::string cmd;
    std::string description;
    std::optional<std::string> error;
};

struct OpResultMessage {
    std::string cmd;
    std::optional<std::string> result;
    std::optional<std::string> error;
};

// ---- 脚本执行 ----
struct RunScriptMessage {
    std::string script_path;
    std::string params;
    std::optional<std::string> session_id;
};

struct ScriptResultMessage {
    bool success;
    std::optional<std::string> result;
    std::optional<std::string> error;
};

// ---- 错误通知 ----
struct ErrorNotifyMessage {
    std::string source;
    std::string severity;
    std::string message;
    std::optional<std::string> details;
};

// ---- 进程控制 ----
// ShutdownMessage：进程间 SHUTDOWN 广播
//   session_id: 可选，标识该 SHUTDOWN 关联的会话（core_engine 侧使用）
//   reason:     受控于 shutdown_reason 命名空间
//   initiator:  受控于 shutdown_initiator 命名空间
struct ShutdownMessage {
    std::optional<std::string> session_id;
    std::string reason;       // 默认空字符串；使用 shutdown_reason::* 常量
    std::string initiator;    // 默认空字符串；使用 shutdown_initiator::* 常量
};

struct EngineDiedMessage {
    std::string session_id;
    std::string reason;
};

// ================================================================
// 序列化/反序列化函数声明
// ================================================================

// 注册
std::string serializeRegister(const RegisterMessage& msg);
std::optional<RegisterMessage> parseRegister(const std::string& json);

// 会话管理
std::string serializeRequestEngine(const RequestEngineMessage& msg);
std::optional<RequestEngineMessage> parseRequestEngine(const std::string& json);
std::string serializeEngineAssigned(const EngineAssignedMessage& msg);
std::optional<EngineAssignedMessage> parseEngineAssigned(const std::string& json);
std::string serializeEngineFailed(const EngineFailedMessage& msg);
std::optional<EngineFailedMessage> parseEngineFailed(const std::string& json);
std::string serializeSessionStateChanged(const SessionStateChangedMessage& msg);
std::optional<SessionStateChangedMessage> parseSessionStateChanged(const std::string& json);
std::string serializeSessionTerminated(const SessionTerminatedMessage& msg);
std::optional<SessionTerminatedMessage> parseSessionTerminated(const std::string& json);
std::string serializeSessionStateUpdate(const SessionStateUpdateMessage& msg);
std::optional<SessionStateUpdateMessage> parseSessionStateUpdate(const std::string& json);

// 初始化会话列表（新增）
std::string serializeInitSessionList(const InitSessionListMessage& msg);
std::optional<InitSessionListMessage> parseInitSessionList(const std::string& json);

// 全量同步
std::string serializeFullSyncRequest(const FullSyncRequestMessage& msg);
std::optional<FullSyncRequestMessage> parseFullSyncRequest(const std::string& json);
std::string serializeFullSyncResponse(const FullSyncResponseMessage& msg);
std::optional<FullSyncResponseMessage> parseFullSyncResponse(const std::string& json);

// 会话注册
std::string serializeRegisterSession(const RegisterSessionMessage& msg);
std::optional<RegisterSessionMessage> parseRegisterSession(const std::string& json);
std::string serializeUnregisterSession(const UnregisterSessionMessage& msg);
std::optional<UnregisterSessionMessage> parseUnregisterSession(const std::string& json);

// 插件相关
std::string serializeInitList(const InitListMessage& msg);
std::optional<InitListMessage> parseInitList(const std::string& json);
std::string serializeInitListAck(const InitListAckMessage& msg);
std::optional<InitListAckMessage> parseInitListAck(const std::string& json);
std::string serializePluginImport(const PluginImportMessage& msg);
std::optional<PluginImportMessage> parsePluginImport(const std::string& json);
std::string serializePluginImportResp(const PluginImportRespMessage& msg);
std::optional<PluginImportRespMessage> parsePluginImportResp(const std::string& json);
std::string serializePluginDelete(const PluginDeleteMessage& msg);
std::optional<PluginDeleteMessage> parsePluginDelete(const std::string& json);
std::string serializePluginDeleteResp(const PluginDeleteRespMessage& msg);
std::optional<PluginDeleteRespMessage> parsePluginDeleteResp(const std::string& json);
std::string serializePluginEnable(const PluginEnableMessage& msg);
std::optional<PluginEnableMessage> parsePluginEnable(const std::string& json);
std::string serializePluginEnableResp(const PluginEnableRespMessage& msg);
std::optional<PluginEnableRespMessage> parsePluginEnableResp(const std::string& json);

// 工具调用
std::string serializeToolCall(const ToolCallMessage& msg);
std::optional<ToolCallMessage> parseToolCall(const std::string& json);
std::string serializeStep(const StepMessage& msg);
std::optional<StepMessage> parseStep(const std::string& json);
std::string serializeOpResult(const OpResultMessage& msg);
std::optional<OpResultMessage> parseOpResult(const std::string& json);

// 脚本执行
std::string serializeRunScript(const RunScriptMessage& msg);
std::optional<RunScriptMessage> parseRunScript(const std::string& json);
std::string serializeScriptResult(const ScriptResultMessage& msg);
std::optional<ScriptResultMessage> parseScriptResult(const std::string& json);

// 错误通知
std::string serializeErrorNotify(const ErrorNotifyMessage& msg);
std::optional<ErrorNotifyMessage> parseErrorNotify(const std::string& json);

// 进程控制
std::string serializeShutdown(const ShutdownMessage& msg);
std::optional<ShutdownMessage> parseShutdown(const std::string& json);
std::string serializeEngineDied(const EngineDiedMessage& msg);
std::optional<EngineDiedMessage> parseEngineDied(const std::string& json);

// ================================================================
// 通用辅助函数
// ================================================================

// 构建消息 JSON。自动写入 version = msg_version::CURRENT。
std::string buildMessage(const std::string& type,
                         const std::string& cmd,
                         const std::string& payload_json);

// 解析消息基字段（兼容版本：忽略 version）
// 保留此签名以保证现有调用点零改动。
bool parseBaseMessage(const std::string& json,
                      std::string& out_type,
                      std::string& out_cmd,
                      std::string& out_payload);

// 解析消息基字段（带版本）
// 用于未来需要版本感知的调用点。缺字段时 out_version 给 CURRENT。
bool parseBaseMessage(const std::string& json,
                      std::string& out_type,
                      std::string& out_cmd,
                      std::string& out_payload,
                      int& out_version);

} // namespace dream_machine