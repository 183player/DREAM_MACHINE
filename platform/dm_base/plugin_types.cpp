// platform/dm_base/plugin_types.cpp
#include "plugin_types.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

#include <string>
#include <optional>

namespace dream_machine::plugin {

// ================================================================
// 辅助：将 QString 转为 std::string
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
// 枚举转换
// ================================================================

std::string pluginTypeToString(PluginType type) {
    switch (type) {
        case PluginType::FRAMEWORK: return "framework";
        case PluginType::COMPONENT: return "component";
        case PluginType::CARD: return "card";
        case PluginType::SHELF_EXTENSION: return "shelf_extension";
    }
    return "component";
}

PluginType stringToPluginType(const std::string& str) {
    if (str == "framework") return PluginType::FRAMEWORK;
    if (str == "component") return PluginType::COMPONENT;
    if (str == "card") return PluginType::CARD;
    if (str == "shelf_extension") return PluginType::SHELF_EXTENSION;
    return PluginType::COMPONENT;
}

std::string modTypeToString(ModificationType type) {
    switch (type) {
        case ModificationType::REPLACE: return "replace";
        case ModificationType::EXTEND: return "extend";
    }
    return "extend";
}

ModificationType stringToModType(const std::string& str) {
    if (str == "replace") return ModificationType::REPLACE;
    return ModificationType::EXTEND;
}

// ================================================================
// PluginManifest 序列化
// ================================================================

std::string manifestToJson(const PluginManifest& manifest) {
    QJsonObject obj;

    obj["id"] = toQString(manifest.id);
    obj["name"] = toQString(manifest.name);
    obj["version"] = toQString(manifest.version);
    obj["system"] = manifest.system;
    obj["type"] = toQString(pluginTypeToString(manifest.type));
    obj["modification"] = toQString(modTypeToString(manifest.modification));

    if (manifest.target.has_value()) {
        QJsonObject targetObj;
        targetObj["file"] = toQString(manifest.target->file);
        targetObj["container"] = toQString(manifest.target->container);
        targetObj["position"] = toQString(manifest.target->position);
        obj["target"] = targetObj;
    }

    QJsonObject entryObj;
    if (!manifest.entry.framework.empty()) {
        entryObj["framework"] = toQString(manifest.entry.framework);
    }
    if (!manifest.entry.components.empty()) {
        QJsonArray comps;
        for (const auto& c : manifest.entry.components) {
            comps.append(toQString(c));
        }
        entryObj["components"] = comps;
    }
    if (!manifest.entry.cards.empty()) {
        QJsonArray cards;
        for (const auto& c : manifest.entry.cards) {
            cards.append(toQString(c));
        }
        entryObj["cards"] = cards;
    }
    if (manifest.entry.global_params.has_value()) {
        entryObj["global_params"] = toQString(*manifest.entry.global_params);
    }
    if (!manifest.entry.scripts.empty()) {
        QJsonArray scripts;
        for (const auto& s : manifest.entry.scripts) {
            scripts.append(toQString(s));
        }
        entryObj["scripts"] = scripts;
    }
    obj["entry"] = entryObj;

    if (!manifest.triggers.empty()) {
        QJsonObject triggersObj;
        for (const auto& pair : manifest.triggers) {
            triggersObj[toQString(pair.first)] = toQString(pair.second);
        }
        obj["triggers"] = triggersObj;
    }

    if (manifest.flow_rules.has_value()) {
        obj["flow_rules"] = toQString(*manifest.flow_rules);
    }

    obj["can_delete"] = manifest.can_delete;
    obj["can_disable"] = manifest.can_disable;
    obj["overridable"] = manifest.overridable;

    QJsonObject policyObj;
    policyObj["allow_function_change"] = manifest.override_policy.allow_function_change;
    policyObj["allow_style_change"] = manifest.override_policy.allow_style_change;
    policyObj["allow_position_change"] = manifest.override_policy.allow_position_change;
    obj["override_policy"] = policyObj;

    obj["sequence"] = static_cast<qint64>(manifest.sequence);
    obj["enabled"] = manifest.enabled;

    QJsonDocument doc(obj);
    return doc.toJson(QJsonDocument::Indented).toStdString();
}

std::optional<PluginManifest> manifestFromJson(const std::string& json) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(QString::fromStdString(json).toUtf8(), &error);
    if (error.error != QJsonParseError::NoError) {
        return std::nullopt;
    }
    if (!doc.isObject()) {
        return std::nullopt;
    }

    QJsonObject obj = doc.object();

    PluginManifest manifest;
    manifest.id = toStdString(obj["id"].toString());
    manifest.name = toStdString(obj["name"].toString());
    manifest.version = toStdString(obj["version"].toString());
    manifest.system = obj["system"].toBool(false);
    manifest.type = stringToPluginType(toStdString(obj["type"].toString()));
    manifest.modification = stringToModType(toStdString(obj["modification"].toString()));

    if (obj.contains("target") && obj["target"].isObject()) {
        QJsonObject targetObj = obj["target"].toObject();
        PluginManifest::Target target;
        target.file = toStdString(targetObj["file"].toString());
        target.container = toStdString(targetObj["container"].toString());
        target.position = toStdString(targetObj["position"].toString());
        manifest.target = target;
    }

    if (obj.contains("entry") && obj["entry"].isObject()) {
        QJsonObject entryObj = obj["entry"].toObject();
        manifest.entry.framework = toStdString(entryObj["framework"].toString());

        if (entryObj.contains("components") && entryObj["components"].isArray()) {
            QJsonArray comps = entryObj["components"].toArray();
            for (const auto& val : comps) {
                manifest.entry.components.push_back(toStdString(val.toString()));
            }
        }
        if (entryObj.contains("cards") && entryObj["cards"].isArray()) {
            QJsonArray cards = entryObj["cards"].toArray();
            for (const auto& val : cards) {
                manifest.entry.cards.push_back(toStdString(val.toString()));
            }
        }
        if (entryObj.contains("global_params")) {
            manifest.entry.global_params = toStdString(entryObj["global_params"].toString());
        }
        if (entryObj.contains("scripts") && entryObj["scripts"].isArray()) {
            QJsonArray scripts = entryObj["scripts"].toArray();
            for (const auto& val : scripts) {
                manifest.entry.scripts.push_back(toStdString(val.toString()));
            }
        }
    }

    if (obj.contains("triggers") && obj["triggers"].isObject()) {
        QJsonObject triggersObj = obj["triggers"].toObject();
        for (auto it = triggersObj.begin(); it != triggersObj.end(); ++it) {
            manifest.triggers[toStdString(it.key())] = toStdString(it.value().toString());
        }
    }

    manifest.flow_rules = optStringFromJson(obj["flow_rules"]);
    manifest.can_delete = obj["can_delete"].toBool(true);
    manifest.can_disable = obj["can_disable"].toBool(true);
    manifest.overridable = obj["overridable"].toBool(true);

    if (obj.contains("override_policy") && obj["override_policy"].isObject()) {
        QJsonObject policyObj = obj["override_policy"].toObject();
        manifest.override_policy.allow_function_change = policyObj["allow_function_change"].toBool(true);
        manifest.override_policy.allow_style_change = policyObj["allow_style_change"].toBool(true);
        manifest.override_policy.allow_position_change = policyObj["allow_position_change"].toBool(true);
    }

    manifest.sequence = static_cast<uint32_t>(obj["sequence"].toInteger(0));
    manifest.enabled = obj["enabled"].toBool(false);

    return manifest;
}

// ================================================================
// InitList 序列化（改为 Compact 格式，避免换行）
// ================================================================

std::string initListToJson(const InitList& list) {
    QJsonObject obj;
    obj["version"] = static_cast<qint64>(list.version);
    obj["generated_at"] = static_cast<qint64>(list.generated_at);

    QJsonArray entriesArray;
    for (const auto& entry : list.entries) {
        QJsonObject entryObj;
        entryObj["type"] = toQString(modTypeToString(entry.type));

        if (entry.type == ModificationType::REPLACE) {
            entryObj["target_file"] = toQString(entry.target_file);
            entryObj["winner_plugin_id"] = toQString(entry.winner_plugin_id);
            entryObj["winner_sequence"] = static_cast<qint64>(entry.winner_sequence);
            entryObj["winner_path"] = toQString(entry.winner_path);
            if (!entry.disabled_plugins.empty()) {
                QJsonArray disabled;
                for (const auto& id : entry.disabled_plugins) {
                    disabled.append(toQString(id));
                }
                entryObj["disabled_plugins"] = disabled;
            }
        }

        if (entry.type == ModificationType::EXTEND) {
            entryObj["container_id"] = toQString(entry.container_id);
            entryObj["plugin_id"] = toQString(entry.plugin_id);
            entryObj["component_file"] = toQString(entry.component_file);
            entryObj["position"] = toQString(entry.position);
            entryObj["enabled"] = entry.enabled;
        }

        if (!entry.trigger.empty()) {
            entryObj["trigger"] = toQString(entry.trigger);
        }
        if (!entry.rule_file.empty()) {
            entryObj["rule_file"] = toQString(entry.rule_file);
        }

        entriesArray.append(entryObj);
    }
    obj["entries"] = entriesArray;

    QJsonDocument doc(obj);
    // 改为 Compact 模式，避免多行导致 readLine 读取不完整
    return doc.toJson(QJsonDocument::Compact).toStdString();
}

std::optional<InitList> initListFromJson(const std::string& json) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(QString::fromStdString(json).toUtf8(), &error);
    if (error.error != QJsonParseError::NoError) {
        return std::nullopt;
    }
    if (!doc.isObject()) {
        return std::nullopt;
    }

    QJsonObject obj = doc.object();

    InitList list;
    list.version = static_cast<uint32_t>(obj["version"].toInteger(1));
    list.generated_at = static_cast<uint64_t>(obj["generated_at"].toInteger(0));

    if (obj.contains("entries") && obj["entries"].isArray()) {
        QJsonArray entriesArray = obj["entries"].toArray();
        for (const auto& val : entriesArray) {
            if (!val.isObject()) continue;
            QJsonObject entryObj = val.toObject();

            InitListEntry entry;
            entry.type = stringToModType(toStdString(entryObj["type"].toString()));

            if (entry.type == ModificationType::REPLACE) {
                entry.target_file = toStdString(entryObj["target_file"].toString());
                entry.winner_plugin_id = toStdString(entryObj["winner_plugin_id"].toString());
                entry.winner_sequence = static_cast<uint32_t>(entryObj["winner_sequence"].toInteger(0));
                entry.winner_path = toStdString(entryObj["winner_path"].toString());
                if (entryObj.contains("disabled_plugins") && entryObj["disabled_plugins"].isArray()) {
                    QJsonArray disabled = entryObj["disabled_plugins"].toArray();
                    for (const auto& d : disabled) {
                        entry.disabled_plugins.push_back(toStdString(d.toString()));
                    }
                }
            } else {
                entry.container_id = toStdString(entryObj["container_id"].toString());
                entry.plugin_id = toStdString(entryObj["plugin_id"].toString());
                entry.component_file = toStdString(entryObj["component_file"].toString());
                entry.position = toStdString(entryObj["position"].toString());
                entry.enabled = entryObj["enabled"].toBool(true);
            }

            entry.trigger = toStdString(entryObj["trigger"].toString());
            entry.rule_file = toStdString(entryObj["rule_file"].toString());

            list.entries.push_back(std::move(entry));
        }
    }

    return list;
}

// ================================================================
// PluginStatus 序列化
// ================================================================

std::string statusToJson(const PluginStatus& status) {
    QJsonObject obj;
    obj["plugin_id"] = toQString(status.plugin_id);
    obj["enabled"] = status.enabled;
    obj["disabled_by_error"] = status.disabled_by_error;
    obj["error_reason"] = toQString(status.error_reason);
    obj["error_timestamp"] = static_cast<qint64>(status.error_timestamp);
    QJsonDocument doc(obj);
    return doc.toJson(QJsonDocument::Indented).toStdString();
}

std::optional<PluginStatus> statusFromJson(const std::string& json) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(QString::fromStdString(json).toUtf8(), &error);
    if (error.error != QJsonParseError::NoError) {
        return std::nullopt;
    }
    if (!doc.isObject()) {
        return std::nullopt;
    }

    QJsonObject obj = doc.object();

    PluginStatus status;
    status.plugin_id = toStdString(obj["plugin_id"].toString());
    status.enabled = obj["enabled"].toBool(false);
    status.disabled_by_error = obj["disabled_by_error"].toBool(false);
    status.error_reason = toStdString(obj["error_reason"].toString());
    status.error_timestamp = static_cast<uint64_t>(obj["error_timestamp"].toInteger(0));

    return status;
}

} // namespace dream_machine::plugin