// platform/dm_base/messages.cpp
#include "messages.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

#include <optional>
#include <string>

namespace dream_machine {

// ================================================================
// 辅助：QString ↔ std::string 转换
// ================================================================
static std::string toStdString(const QString& qstr) {
    return qstr.toStdString();
}

static QString toQString(const std::string& str) {
    return QString::fromStdString(str);
}

// ================================================================
// 辅助：QJsonValue 转 std::optional<std::string>
// ================================================================
static std::optional<std::string> optStringFromJson(const QJsonValue& val) {
    if (val.isUndefined() || val.isNull()) {
        return std::nullopt;
    }
    return toStdString(val.toString());
}

// ================================================================
// 辅助：构建标准消息
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
// 序列化：各消息类型
// ================================================================

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

std::string serializeRequestEngine(const RequestEngineMessage& msg) {
    QJsonObject obj;
    obj["session_id"] = toQString(msg.session_id);
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::REQUEST_ENGINE, "", payload);
}

std::string serializeEngineAssigned(const EngineAssignedMessage& msg) {
    QJsonObject obj;
    obj["session_id"] = toQString(msg.session_id);
    obj["pipe_name"] = toQString(msg.pipe_name);
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::ENGINE_ASSIGNED, "", payload);
}

std::string serializeEngineFailed(const EngineFailedMessage& msg) {
    QJsonObject obj;
    obj["session_id"] = toQString(msg.session_id);
    obj["reason"] = toQString(msg.reason);
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::ENGINE_FAILED, "", payload);
}

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

std::string serializeSessionStateUpdate(const SessionStateUpdateMessage& msg) {
    QJsonObject obj;
    obj["session_id"] = toQString(msg.session_id);
    obj["state"] = toQString(msg.state);
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::SESSION_STATE_UPDATE, "", payload);
}

std::string serializeInitList(const InitListMessage& msg) {
    // payload 就是初始化列表的 JSON 字符串（插件管理器已生成）
    return buildMessage(msg_types::INIT_LIST, "", msg.list_json);
}

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

std::string serializePluginImport(const PluginImportMessage& msg) {
    QJsonObject obj;
    obj["package_path"] = toQString(msg.package_path);
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::PLUGIN_IMPORT, "", payload);
}

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

std::string serializePluginDelete(const PluginDeleteMessage& msg) {
    QJsonObject obj;
    obj["plugin_id"] = toQString(msg.plugin_id);
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::PLUGIN_DELETE, "", payload);
}

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

std::string serializePluginEnable(const PluginEnableMessage& msg) {
    QJsonObject obj;
    obj["plugin_id"] = toQString(msg.plugin_id);
    obj["enabled"] = msg.enabled;
    QJsonDocument doc(obj);
    std::string payload = doc.toJson(QJsonDocument::Compact).toStdString();
    return buildMessage(msg_types::PLUGIN_ENABLE, "", payload);
}

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

// ================================================================
// 反序列化：各消息类型
// ================================================================

static bool parseSimplePayload(const std::string& json, QJsonObject& out_obj) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(toQString(json).toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    out_obj = doc.object();
    return true;
}

std::optional<RegisterMessage> parseRegister(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;

    RegisterMessage msg;
    msg.process = toStdString(obj["process"].toString());
    msg.session_id = optStringFromJson(obj["session_id"]);
    return msg;
}

std::optional<RequestEngineMessage> parseRequestEngine(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;

    RequestEngineMessage msg;
    msg.session_id = toStdString(obj["session_id"].toString());
    return msg;
}

std::optional<EngineAssignedMessage> parseEngineAssigned(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;

    EngineAssignedMessage msg;
    msg.session_id = toStdString(obj["session_id"].toString());
    msg.pipe_name = toStdString(obj["pipe_name"].toString());
    return msg;
}

std::optional<EngineFailedMessage> parseEngineFailed(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;

    EngineFailedMessage msg;
    msg.session_id = toStdString(obj["session_id"].toString());
    msg.reason = toStdString(obj["reason"].toString());
    return msg;
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

std::optional<SessionStateUpdateMessage> parseSessionStateUpdate(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;

    SessionStateUpdateMessage msg;
    msg.session_id = toStdString(obj["session_id"].toString());
    msg.state = toStdString(obj["state"].toString());
    return msg;
}

// ================================================================
// 修正：parseInitList - 正确处理对象类型的 "list" 字段
// ================================================================
std::optional<InitListMessage> parseInitList(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) {
        return std::nullopt;
    }

    InitListMessage msg;

    if (obj.contains("list")) {
        QJsonValue listVal = obj["list"];
        if (listVal.isObject()) {
            // "list" 字段是一个 JSON 对象 → 序列化为字符串
            QJsonDocument doc(listVal.toObject());
            msg.list_json = doc.toJson(QJsonDocument::Compact).toStdString();
        } else if (listVal.isString()) {
            // "list" 字段是一个字符串 → 直接使用
            msg.list_json = toStdString(listVal.toString());
        } else {
            // 其他情况：整个 payload 作为 JSON 字符串（回退）
            QJsonDocument doc(obj);
            msg.list_json = doc.toJson(QJsonDocument::Compact).toStdString();
        }
    } else {
        // 没有 "list" 字段，整个 payload 就是列表 JSON
        QJsonDocument doc(obj);
        msg.list_json = doc.toJson(QJsonDocument::Compact).toStdString();
    }

    return msg;
}

std::optional<InitListAckMessage> parseInitListAck(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;

    InitListAckMessage msg;
    msg.status = toStdString(obj["status"].toString());
    msg.error = optStringFromJson(obj["error"]);
    return msg;
}

std::optional<PluginImportMessage> parsePluginImport(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;

    PluginImportMessage msg;
    msg.package_path = toStdString(obj["package_path"].toString());
    return msg;
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

std::optional<PluginDeleteMessage> parsePluginDelete(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;

    PluginDeleteMessage msg;
    msg.plugin_id = toStdString(obj["plugin_id"].toString());
    return msg;
}

std::optional<PluginDeleteRespMessage> parsePluginDeleteResp(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;

    PluginDeleteRespMessage msg;
    msg.success = obj["success"].toBool(false);
    msg.error = optStringFromJson(obj["error"]);
    return msg;
}

std::optional<PluginEnableMessage> parsePluginEnable(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;

    PluginEnableMessage msg;
    msg.plugin_id = toStdString(obj["plugin_id"].toString());
    msg.enabled = obj["enabled"].toBool(false);
    return msg;
}

std::optional<PluginEnableRespMessage> parsePluginEnableResp(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;

    PluginEnableRespMessage msg;
    msg.success = obj["success"].toBool(false);
    msg.error = optStringFromJson(obj["error"]);
    return msg;
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

std::optional<StepMessage> parseStep(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;

    StepMessage msg;
    msg.cmd = toStdString(obj["cmd"].toString());
    msg.description = toStdString(obj["description"].toString());
    msg.error = optStringFromJson(obj["error"]);
    return msg;
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

std::optional<RunScriptMessage> parseRunScript(const std::string& json) {
    QJsonObject obj;
    if (!parseSimplePayload(json, obj)) return std::nullopt;

    RunScriptMessage msg;
    msg.script_path = toStdString(obj["script_path"].toString());
    msg.params = toStdString(obj["params"].toString());
    msg.session_id = optStringFromJson(obj["session_id"]);
    return msg;
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

} // namespace dream_machine