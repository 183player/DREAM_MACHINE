// platform/dm_base/messages.cpp
#include "messages.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

#include <optional>
#include <string>
#include <vector>

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

std::string buildMessage(const std::string& type,
                         const std::string& cmd,
                         const std::string& payload_json) {
    QJsonObject obj;
    obj["type"] = toQString(type);
    if (!cmd.empty()) {
        obj["cmd"] = toQString(cmd);
    }
    obj["payload"] = toQString(payload_json);
    QJsonDocument doc(obj);
    return doc.toJson(QJsonDocument::Compact).toStdString();
}

bool parseBaseMessage(const std::string& json,
                      std::string& out_type,
                      std::string& out_cmd,
                      std::string& out_payload) {
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
    return true;
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
std::string serializeShutdown(const ShutdownMessage& msg) {
    QJsonObject obj;
    if (msg.session_id.has_value()) {
        obj["session_id"] = toQString(*msg.session_id);
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