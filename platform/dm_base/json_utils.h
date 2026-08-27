// platform/dm_base/json_utils.h
#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <optional>
#include <string>

namespace dream_machine {

// ============================================================================
// JSON 解析结果
// ============================================================================

struct JsonParseResult {
    bool ok = false;
    QJsonDocument document;
    std::string error_message;

    explicit operator bool() const { return ok; }
};

// ============================================================================
// 解析函数
// ============================================================================

inline JsonParseResult parseJson(const std::string& json_str) {
    JsonParseResult result;
    QJsonParseError parse_error;
    result.document = QJsonDocument::fromJson(QString::fromStdString(json_str).toUtf8(), &parse_error);

    if (parse_error.error == QJsonParseError::NoError) {
        result.ok = true;
    } else {
        result.ok = false;
        result.error_message = parse_error.errorString().toStdString();
    }
    return result;
}

inline std::optional<QJsonObject> parseJsonObject(const std::string& json_str) {
    auto result = parseJson(json_str);
    if (!result.ok || !result.document.isObject()) {
        return std::nullopt;
    }
    return result.document.object();
}

inline std::optional<QJsonArray> parseJsonArray(const std::string& json_str) {
    auto result = parseJson(json_str);
    if (!result.ok || !result.document.isArray()) {
        return std::nullopt;
    }
    return result.document.array();
}

inline std::string stringifyJson(const QJsonDocument& doc, bool compact = true) {
    return doc.toJson(compact ? QJsonDocument::Compact : QJsonDocument::Indented).toStdString();
}

// ============================================================================
// QJsonObject 安全读取
// ============================================================================

inline std::optional<std::string> getStringField(const QJsonObject& obj, const QString& key) {
    if (!obj.contains(key) || !obj[key].isString()) {
        return std::nullopt;
    }
    return obj[key].toString().toStdString();
}

inline std::optional<int> getIntField(const QJsonObject& obj, const QString& key) {
    if (!obj.contains(key) || !obj[key].isDouble()) {
        return std::nullopt;
    }
    return obj[key].toInt();
}

inline std::optional<bool> getBoolField(const QJsonObject& obj, const QString& key) {
    if (!obj.contains(key) || !obj[key].isBool()) {
        return std::nullopt;
    }
    return obj[key].toBool();
}

inline std::optional<QJsonObject> getObjectField(const QJsonObject& obj, const QString& key) {
    if (!obj.contains(key) || !obj[key].isObject()) {
        return std::nullopt;
    }
    return obj[key].toObject();
}

inline std::optional<QJsonArray> getArrayField(const QJsonObject& obj, const QString& key) {
    if (!obj.contains(key) || !obj[key].isArray()) {
        return std::nullopt;
    }
    return obj[key].toArray();
}

} // namespace dream_machine