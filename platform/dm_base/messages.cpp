// platform/dm_base/messages.cpp
#include "messages.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

#include <optional>
#include <string>
#include <vector>

#include "logger.h"   // A.9：版本兼容性 WARN；A.10：request_id 读取
#include <atomic>     // A.9：static atomic flag

namespace dream_machine {

// ================================================================
// 辅助：QString ↔ std::string
// ================================================================
static std::string toStdString(const QString& qstr) {
    return qstr.toStdString();
}

static QString toQString(const std::string& str) {
    return QString::fromStdString(str);
}

static std::optional<std::string> optStringFromJson(const QJsonValue& val) {
    if (val.isUndefined() || val.isNull()) {
        return std::nullopt;
    }
    return toStdString(val.toString());
}

static bool parseSimplePayload(const std::string& json, QJsonObject& out_obj) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(toQString(json).toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    out_obj = doc.object();
    return true;
}

// ================================================================
// buildMessage / parseBaseMessage
// ================================================================

// 构建消息 JSON
//
// 自动写入：
//   - version    = msg_version::CURRENT
//   - request_id = Logger::getRequestId()（非空时；A.10 D2）
//
// A.10 D2 说明：
//   - request_id 由 Logger thread_local 提供，各进程通过
//     Logger::setRequestId / clearRequestId 控制
//   - request_id 为空时 JSON 不写字段，向后兼容
//   - 各 serializeXxx 内部调用本函数，因此零改动即自动携带
std::string buildMessage(const std::string& type,
                         const std::string& cmd,
                         const std::string& payload_json) {
    QJsonObject obj;
    obj["type"] = toQString(type);
    if (!cmd.empty()) {
        obj["cmd"] = toQString(cmd);
    }
    obj["payload"] = toQString(payload_json);
    obj["version"] = msg_version::CURRENT;

    // A.10 D2：自动携带当前线程的 request_id（非空时）
    const std::string req = Logger::getRequestId();
    if (!req.empty()) {
        obj["request_id"] = toQString(req);
    }

    QJsonDocument doc(obj);
    return doc.toJson(QJsonDocument::Compact).toStdString();
}

// 解析消息基字段（带版本 + request_id）
//
// 依据阶段 1.7 B4 + A.9 最小协商 + A.10 D2：
//   - version 字段缺失时给 CURRENT（兼容旧消息，不产生噪声）
//   - request_id 字段缺失时给空字符串（兼容旧消息）
//   - 版本不兼容时仅记录 WARN（首次），不拒绝（保守策略）
//   - 不做版本回退、不做降级
//   - 不自动 setRequestId（由调用方决定）
bool parseBaseMessage(const std::string& json,
                      std::string& out_type,
                      std::string& out_cmd,
                      std::string& out_payload,
                      int& out_version,
                      std::string& out_request_id) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(toQString(json).toUtf8(), &error);
    if (error.error != QJsonParseError::NoError) {
        return false;
    }
    if (!doc.isObject()) {
        return false;
    }
    QJsonObject obj = doc.object();
    out_type = toStdString(obj["type"].toString());
    out_cmd = toStdString(obj["cmd"].toString());
    out_payload = toStdString(obj["payload"].toString());

    // version：兼容缺失字段，给默认当前版本
    if (obj.contains("version") && obj["version"].isDouble()) {
        out_version = obj["version"].toInt(msg_version::CURRENT);
    } else {
        out_version = msg_version::CURRENT;
    }

    // A.10 D2：request_id（可选字段）
    //   - 存在且为 string → 读取
    //   - 缺失或类型不符 → 空字符串
    //   - 不自动 setRequestId（由调用方决定）
    if (obj.contains("request_id") && obj["request_id"].isString()) {
        out_request_id = toStdString(obj["request_id"].toString());
    } else {
        out_request_id.clear();
    }

    // ----- A.9：版本兼容性检查（最小协商） -----
    //
    // 语义：
    //   - 对端 version 在 [MIN_SUPPORTED, CURRENT] 范围内 → 兼容，正常处理
    //   - 其他情况 → 不兼容
    //
    // 不兼容处理（保守策略）：
    //   - 仅记录一次 WARN（static atomic flag 防止刷屏）
    //   - 仍继续处理消息（不拒绝、不回退、不降级）
    if (!msg_version::isCompatible(out_version)) {
        static std::atomic<bool> version_warned{false};
        if (!version_warned.exchange(true)) {
            LOG_WARN("Message version incompatible: remote=" +
                     std::to_string(out_version) +
                     ", supported=[" +
                     std::to_string(msg_version::MIN_SUPPORTED) + ", " +
                     std::to_string(msg_version::CURRENT) + "]" +
                     " — continuing anyway (conservative)");
        }
    }

    return true;
}

// 解析消息基字段（带版本，忽略 request_id）
//
// 委托给带 request_id 的重载，忽略 request_id 输出。
bool parseBaseMessage(const std::string& json,
                      std::string& out_type,
                      std::string& out_cmd,
                      std::string& out_payload,
                      int& out_version) {
    std::string ignored_request_id;
    return parseBaseMessage(json, out_type, out_cmd, out_payload,
                            out_version, ignored_request_id);
}

// 解析消息基字段（兼容签名：忽略 version 与 request_id）
//
// 保留此签名以保证现有调用点零改动；
// 内部委托给带 version 的重载（该重载会做版本兼容性检查）。
bool parseBaseMessage(const std::string& json,
                      std::string& out_type,
                      std::string& out_cmd,
                      std::string& out_payload) {
    int ignored_version = 0;
    return parseBaseMessage(json, out_type, out_cmd, out_payload, ignored_version);
}

// ================================================================
// 序列化/反序列化
// ================================================================

// ---- Register ----
std::string serializeRegister(const RegisterMessage& msg) {
    QJsonObject obj;
    obj["process"] = toQString(msg.process);
    if (msg.session_id.has_value()) {
        obj["session_id"] = toQString(*msg.session_id);
    }
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::REGISTER, "", payload);
}

std::optional<RegisterMessage> parseRegister(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    RegisterMessage msg;
    msg.process = toStdString(obj["process"].toString());
    msg.session_id = optStringFromJson(obj["session_id"]);
    return msg;
}

// ---- RequestEngine ----
std::string serializeRequestEngine(const RequestEngineMessage& msg) {
    QJsonObject obj;
    obj["session_id"] = toQString(msg.session_id);
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::REQUEST_ENGINE, "", payload);
}

std::optional<RequestEngineMessage> parseRequestEngine(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    RequestEngineMessage msg;
    msg.session_id = toStdString(obj["session_id"].toString());
    return msg;
}

// ---- EngineAssigned ----
std::string serializeEngineAssigned(const EngineAssignedMessage& msg) {
    QJsonObject obj;
    obj["session_id"] = toQString(msg.session_id);
    obj["pipe_name"] = toQString(msg.pipe_name);
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::ENGINE_ASSIGNED, "", payload);
}

std::optional<EngineAssignedMessage> parseEngineAssigned(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    EngineAssignedMessage msg;
    msg.session_id = toStdString(obj["session_id"].toString());
    msg.pipe_name = toStdString(obj["pipe_name"].toString());
    return msg;
}

// ---- EngineFailed ----
std::string serializeEngineFailed(const EngineFailedMessage& msg) {
    QJsonObject obj;
    obj["session_id"] = toQString(msg.session_id);
    obj["reason"] = toQString(msg.reason);
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::ENGINE_FAILED, "", payload);
}

std::optional<EngineFailedMessage> parseEngineFailed(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    EngineFailedMessage msg;
    msg.session_id = toStdString(obj["session_id"].toString());
    msg.reason = toStdString(obj["reason"].toString());
    return msg;
}

// ---- SessionStateChanged ----
std::string serializeSessionStateChanged(const SessionStateChangedMessage& msg) {
    QJsonObject obj;
    obj["session_id"] = toQString(msg.session_id);
    obj["state"] = toQString(msg.state);
    if (msg.pipe_name.has_value()) {
        obj["pipe_name"] = toQString(*msg.pipe_name);
    }
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::SESSION_STATE_CHANGED, "", payload);
}

std::optional<SessionStateChangedMessage> parseSessionStateChanged(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    SessionStateChangedMessage msg;
    msg.session_id = toStdString(obj["session_id"].toString());
    msg.state = toStdString(obj["state"].toString());
    msg.pipe_name = optStringFromJson(obj["pipe_name"]);
    return msg;
}

// ---- SessionTerminated ----
std::string serializeSessionTerminated(const SessionTerminatedMessage& msg) {
    QJsonObject obj;
    obj["session_id"] = toQString(msg.session_id);
    obj["reason"] = toQString(msg.reason);
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::SESSION_TERMINATED, "", payload);
}

std::optional<SessionTerminatedMessage> parseSessionTerminated(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    SessionTerminatedMessage msg;
    msg.session_id = toStdString(obj["session_id"].toString());
    msg.reason = toStdString(obj["reason"].toString());
    return msg;
}

// ---- SessionStateUpdate ----
std::string serializeSessionStateUpdate(const SessionStateUpdateMessage& msg) {
    QJsonObject obj;
    obj["session_id"] = toQString(msg.session_id);
    obj["state"] = toQString(msg.state);
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::SESSION_STATE_UPDATE, "", payload);
}

std::optional<SessionStateUpdateMessage> parseSessionStateUpdate(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    SessionStateUpdateMessage msg;
    msg.session_id = toStdString(obj["session_id"].toString());
    msg.state = toStdString(obj["state"].toString());
    return msg;
}

// ---- InitSessionList（新增） ----
std::string serializeInitSessionList(const InitSessionListMessage& msg) {
    QJsonObject obj;
    QJsonArray arr;
    for (const auto& s : msg.sessions) {
        QJsonObject entry;
        entry["session_id"] = toQString(s.session_id);
        entry["state"] = toQString(s.state);
        arr.append(entry);
    }
    obj["sessions"] = arr;
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::INIT_SESSION_LIST, "", payload);
}

std::optional<InitSessionListMessage> parseInitSessionList(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    InitSessionListMessage msg;
    if (obj.contains("sessions") && obj["sessions"].isArray()) {
        QJsonArray arr = obj["sessions"].toArray();
        for (const auto& val : arr) {
            if (!val.isObject()) continue;
            QJsonObject entry = val.toObject();
            SessionStateUpdateMessage s;
            s.session_id = toStdString(entry["session_id"].toString());
            s.state = toStdString(entry["state"].toString());
            msg.sessions.push_back(s);
        }
    }
    return msg;
}

// ---- RegisterSession ----
std::string serializeRegisterSession(const RegisterSessionMessage& msg) {
    QJsonObject obj;
    obj["session_id"] = toQString(msg.session_id);
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::REGISTER_SESSION, "", payload);
}

std::optional<RegisterSessionMessage> parseRegisterSession(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    RegisterSessionMessage msg;
    msg.session_id = toStdString(obj["session_id"].toString());
    return msg;
}

// ---- UnregisterSession ----
std::string serializeUnregisterSession(const UnregisterSessionMessage& msg) {
    QJsonObject obj;
    obj["session_id"] = toQString(msg.session_id);
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::UNREGISTER_SESSION, "", payload);
}

std::optional<UnregisterSessionMessage> parseUnregisterSession(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    UnregisterSessionMessage msg;
    msg.session_id = toStdString(obj["session_id"].toString());
    return msg;
}

// ---- FullSyncRequest ----
std::string serializeFullSyncRequest(const FullSyncRequestMessage& msg) {
    QJsonObject obj;
    obj["request_id"] = static_cast<qint64>(msg.request_id);
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::FULL_SYNC_REQUEST, "", payload);
}

std::optional<FullSyncRequestMessage> parseFullSyncRequest(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    FullSyncRequestMessage msg;
    msg.request_id = obj["request_id"].toInteger(0);
    return msg;
}

// ---- FullSyncResponse ----
std::string serializeFullSyncResponse(const FullSyncResponseMessage& msg) {
    QJsonObject obj;
    obj["request_id"] = static_cast<qint64>(msg.request_id);
    QJsonArray arr;
    for (const auto& s : msg.sessions) {
        QJsonObject entry;
        entry["session_id"] = toQString(s.session_id);
        entry["state"] = toQString(s.state);
        if (s.pipe_name.has_value()) {
            entry["pipe_name"] = toQString(*s.pipe_name);
        }
        arr.append(entry);
    }
    obj["sessions"] = arr;
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::FULL_SYNC_RESPONSE, "", payload);
}

std::optional<FullSyncResponseMessage> parseFullSyncResponse(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    FullSyncResponseMessage msg;
    msg.request_id = obj["request_id"].toInteger(0);
    if (obj.contains("sessions") && obj["sessions"].isArray()) {
        QJsonArray arr = obj["sessions"].toArray();
        for (const auto& val : arr) {
            if (!val.isObject()) continue;
            QJsonObject entry = val.toObject();
            SessionStateChangedMessage s;
            s.session_id = toStdString(entry["session_id"].toString());
            s.state = toStdString(entry["state"].toString());
            s.pipe_name = optStringFromJson(entry["pipe_name"]);
            msg.sessions.push_back(s);
        }
    }
    return msg;
}

// ---- InitList ----
std::string serializeInitList(const InitListMessage& msg) {
    return buildMessage(msg_types::INIT_LIST, "", msg.list_json);
}

std::optional<InitListMessage> parseInitList(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    InitListMessage msg;
    if (obj.contains("list")) {
        QJsonValue val = obj["list"];
        if (val.isObject()) {
            QJsonDocument doc(val.toObject());
            msg.list_json = doc.toJson(QJsonDocument::Compact).toStdString();
        } else if (val.isString()) {
            msg.list_json = toStdString(val.toString());
        } else {
            QJsonDocument doc(obj);
            msg.list_json = doc.toJson(QJsonDocument::Compact).toStdString();
        }
    } else {
        QJsonDocument doc(obj);
        msg.list_json = doc.toJson(QJsonDocument::Compact).toStdString();
    }
    return msg;
}

// ---- InitListAck ----
std::string serializeInitListAck(const InitListAckMessage& msg) {
    QJsonObject obj;
    obj["status"] = toQString(msg.status);
    if (msg.error.has_value()) {
        obj["error"] = toQString(*msg.error);
    }
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::INIT_LIST_ACK, "", payload);
}

std::optional<InitListAckMessage> parseInitListAck(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    InitListAckMessage msg;
    msg.status = toStdString(obj["status"].toString());
    msg.error = optStringFromJson(obj["error"]);
    return msg;
}

// ---- PluginImport ----
std::string serializePluginImport(const PluginImportMessage& msg) {
    QJsonObject obj;
    obj["package_path"] = toQString(msg.package_path);
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::PLUGIN_IMPORT, "", payload);
}

std::optional<PluginImportMessage> parsePluginImport(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    PluginImportMessage msg;
    msg.package_path = toStdString(obj["package_path"].toString());
    return msg;
}

// ---- PluginImportResp ----
std::string serializePluginImportResp(const PluginImportRespMessage& msg) {
    QJsonObject obj;
    obj["success"] = msg.success;
    if (msg.plugin_id.has_value()) {
        obj["plugin_id"] = toQString(*msg.plugin_id);
    }
    if (msg.error.has_value()) {
        obj["error"] = toQString(*msg.error);
    }
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::PLUGIN_IMPORT_RESP, "", payload);
}

std::optional<PluginImportRespMessage> parsePluginImportResp(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    PluginImportRespMessage msg;
    msg.success = obj["success"].toBool(false);
    msg.plugin_id = optStringFromJson(obj["plugin_id"]);
    msg.error = optStringFromJson(obj["error"]);
    return msg;
}

// ---- PluginDelete ----
std::string serializePluginDelete(const PluginDeleteMessage& msg) {
    QJsonObject obj;
    obj["plugin_id"] = toQString(msg.plugin_id);
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::PLUGIN_DELETE, "", payload);
}

std::optional<PluginDeleteMessage> parsePluginDelete(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    PluginDeleteMessage msg;
    msg.plugin_id = toStdString(obj["plugin_id"].toString());
    return msg;
}

// ---- PluginDeleteResp ----
std::string serializePluginDeleteResp(const PluginDeleteRespMessage& msg) {
    QJsonObject obj;
    obj["success"] = msg.success;
    if (msg.error.has_value()) {
        obj["error"] = toQString(*msg.error);
    }
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::PLUGIN_DELETE_RESP, "", payload);
}

std::optional<PluginDeleteRespMessage> parsePluginDeleteResp(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    PluginDeleteRespMessage msg;
    msg.success = obj["success"].toBool(false);
    msg.error = optStringFromJson(obj["error"]);
    return msg;
}

// ---- PluginEnable ----
std::string serializePluginEnable(const PluginEnableMessage& msg) {
    QJsonObject obj;
    obj["plugin_id"] = toQString(msg.plugin_id);
    obj["enabled"] = msg.enabled;
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::PLUGIN_ENABLE, "", payload);
}

std::optional<PluginEnableMessage> parsePluginEnable(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    PluginEnableMessage msg;
    msg.plugin_id = toStdString(obj["plugin_id"].toString());
    msg.enabled = obj["enabled"].toBool(false);
    return msg;
}

// ---- PluginEnableResp ----
std::string serializePluginEnableResp(const PluginEnableRespMessage& msg) {
    QJsonObject obj;
    obj["success"] = msg.success;
    if (msg.error.has_value()) {
        obj["error"] = toQString(*msg.error);
    }
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::PLUGIN_ENABLE_RESP, "", payload);
}

std::optional<PluginEnableRespMessage> parsePluginEnableResp(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    PluginEnableRespMessage msg;
    msg.success = obj["success"].toBool(false);
    msg.error = optStringFromJson(obj["error"]);
    return msg;
}

// ---- ToolCall ----
std::string serializeToolCall(const ToolCallMessage& msg) {
    QJsonObject obj;
    obj["tool"] = toQString(msg.tool);
    obj["params"] = toQString(msg.params);
    if (msg.session_id.has_value()) {
        obj["session_id"] = toQString(*msg.session_id);
    }
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::TOOL_CALL, "", payload);
}

std::optional<ToolCallMessage> parseToolCall(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    ToolCallMessage msg;
    msg.tool = toStdString(obj["tool"].toString());
    msg.params = toStdString(obj["params"].toString());
    msg.session_id = optStringFromJson(obj["session_id"]);
    return msg;
}

// ---- Step ----
std::string serializeStep(const StepMessage& msg) {
    QJsonObject obj;
    obj["cmd"] = toQString(msg.cmd);
    obj["description"] = toQString(msg.description);
    if (msg.error.has_value()) {
        obj["error"] = toQString(*msg.error);
    }
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::STEP_START, "", payload);
}

std::optional<StepMessage> parseStep(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    StepMessage msg;
    msg.cmd = toStdString(obj["cmd"].toString());
    msg.description = toStdString(obj["description"].toString());
    msg.error = optStringFromJson(obj["error"]);
    return msg;
}

// ---- OpResult ----
std::string serializeOpResult(const OpResultMessage& msg) {
    QJsonObject obj;
    obj["cmd"] = toQString(msg.cmd);
    if (msg.result.has_value()) {
        obj["result"] = toQString(*msg.result);
    }
    if (msg.error.has_value()) {
        obj["error"] = toQString(*msg.error);
    }
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::OP_DONE, "", payload);
}

std::optional<OpResultMessage> parseOpResult(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    OpResultMessage msg;
    msg.cmd = toStdString(obj["cmd"].toString());
    msg.result = optStringFromJson(obj["result"]);
    msg.error = optStringFromJson(obj["error"]);
    return msg;
}

// ---- RunScript ----
std::string serializeRunScript(const RunScriptMessage& msg) {
    QJsonObject obj;
    obj["script_path"] = toQString(msg.script_path);
    obj["params"] = toQString(msg.params);
    if (msg.session_id.has_value()) {
        obj["session_id"] = toQString(*msg.session_id);
    }
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::RUN_SCRIPT, "", payload);
}

std::optional<RunScriptMessage> parseRunScript(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    RunScriptMessage msg;
    msg.script_path = toStdString(obj["script_path"].toString());
    msg.params = toStdString(obj["params"].toString());
    msg.session_id = optStringFromJson(obj["session_id"]);
    return msg;
}

// ---- ScriptResult ----
std::string serializeScriptResult(const ScriptResultMessage& msg) {
    QJsonObject obj;
    obj["success"] = msg.success;
    if (msg.result.has_value()) {
        obj["result"] = toQString(*msg.result);
    }
    if (msg.error.has_value()) {
        obj["error"] = toQString(*msg.error);
    }
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::SCRIPT_RESULT, "", payload);
}

std::optional<ScriptResultMessage> parseScriptResult(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    ScriptResultMessage msg;
    msg.success = obj["success"].toBool(false);
    msg.result = optStringFromJson(obj["result"]);
    msg.error = optStringFromJson(obj["error"]);
    return msg;
}

// ---- ErrorNotify ----
std::string serializeErrorNotify(const ErrorNotifyMessage& msg) {
    QJsonObject obj;
    obj["source"] = toQString(msg.source);
    obj["severity"] = toQString(msg.severity);
    obj["message"] = toQString(msg.message);
    if (msg.details.has_value()) {
        obj["details"] = toQString(*msg.details);
    }
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::ERROR_NOTIFY, "", payload);
}

std::optional<ErrorNotifyMessage> parseErrorNotify(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    ErrorNotifyMessage msg;
    msg.source = toStdString(obj["source"].toString());
    msg.severity = toStdString(obj["severity"].toString());
    msg.message = toStdString(obj["message"].toString());
    msg.details = optStringFromJson(obj["details"]);
    return msg;
}

// ---- Shutdown ----
//
// 扩展说明（契约 #15）：
//   reason  / initiator 为受控字符串，非空才写入 JSON，
//   与同文件的 session_id / pipe_name 风格一致。
//   解析侧对缺字段给默认空字符串，保证不返回 nullopt。
//
std::string serializeShutdown(const ShutdownMessage& msg) {
    QJsonObject obj;
    if (msg.session_id.has_value()) {
        obj["session_id"] = toQString(*msg.session_id);
    }
    if (!msg.reason.empty()) {
        obj["reason"] = toQString(msg.reason);
    }
    if (!msg.initiator.empty()) {
        obj["initiator"] = toQString(msg.initiator);
    }
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::SHUTDOWN, "", payload);
}

std::optional<ShutdownMessage> parseShutdown(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    ShutdownMessage msg;
    msg.session_id = optStringFromJson(obj["session_id"]);
    msg.reason = optStringFromJson(obj["reason"]).value_or("");
    msg.initiator = optStringFromJson(obj["initiator"]).value_or("");
    return msg;
}

// ---- EngineDied ----
std::string serializeEngineDied(const EngineDiedMessage& msg) {
    QJsonObject obj;
    obj["session_id"] = toQString(msg.session_id);
    obj["reason"] = toQString(msg.reason);
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::ENGINE_DIED, "", payload);
}

std::optional<EngineDiedMessage> parseEngineDied(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;
    EngineDiedMessage msg;
    msg.session_id = toStdString(obj["session_id"].toString());
    msg.reason = toStdString(obj["reason"].toString());
    return msg;
}

} // namespace dream_machine