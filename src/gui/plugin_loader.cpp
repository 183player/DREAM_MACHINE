// src/gui/plugin_loader.cpp
#include "plugin_loader.h"

#include "logger.h"

#include <QUrl>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <string>

namespace dream_machine::gui {

// ================================================================
// 构造函数 / 析构函数
// ================================================================

PluginLoader::PluginLoader() = default;

PluginLoader::~PluginLoader() {
    reset();
}

// ================================================================
// 初始化
// ================================================================

void PluginLoader::setEngine(QQmlApplicationEngine* engine) {
    engine_ = engine;
}

// ================================================================
// 加载单个 QML 文件（const）
// ================================================================

QObject* PluginLoader::loadQmlFile(const QString& file_path, QObject* parent) const {
    if (!engine_) {
        LOG_ERROR("QML engine not set");
        return nullptr;
    }

    if (!QFile::exists(file_path)) {
        LOG_ERROR("QML file not found: " + file_path.toStdString());
        return nullptr;
    }

    QQmlComponent component(engine_, QUrl::fromLocalFile(file_path));
    if (component.isError()) {
        for (const auto& err : component.errors()) {
            LOG_ERROR("QML component error: " + err.toString().toStdString());
        }
        return nullptr;
    }

    QObject* obj = component.create();
    if (!obj) {
        LOG_ERROR("Failed to create QML object from: " + file_path.toStdString());
        return nullptr;
    }

    if (parent) {
        obj->setParent(parent);
    }

    LOG_INFO("Loaded QML: " + file_path.toStdString());
    return obj;
}

// ================================================================
// 加载 Framework
// ================================================================

bool PluginLoader::loadFramework(const std::string& file_path) {
    if (!engine_) {
        LOG_ERROR("QML engine not set");
        return false;
    }

    if (framework_loaded_ && framework_obj_) {
        framework_obj_->deleteLater();
        framework_obj_ = nullptr;
        framework_loaded_ = false;
    }

    QObject* obj = loadQmlFile(QString::fromStdString(file_path));
    if (!obj) {
        LOG_ERROR("Failed to load framework: " + file_path);
        return false;
    }

    framework_obj_ = obj;
    framework_loaded_ = true;

    LOG_INFO("Framework loaded: " + file_path);
    return true;
}

// ================================================================
// 查找容器（const）
// ================================================================

QQuickItem* PluginLoader::findContainer(const std::string& container_id) const {
    if (!engine_) {
        LOG_ERROR("QML engine not set");
        return nullptr;
    }

    QObject* root = engine_->rootObjects().value(0);
    if (!root) {
        LOG_WARN("No root object found");
        return nullptr;
    }

    QObject* containerObj = root->findChild<QObject*>(
        QString::fromStdString(container_id)
    );
    if (!containerObj) {
        LOG_WARN("Container not found: " + container_id);
        return nullptr;
    }

    auto* containerItem = qobject_cast<QQuickItem*>(containerObj);
    if (!containerItem) {
        LOG_WARN("Container is not a QQuickItem: " + container_id);
        return nullptr;
    }

    return containerItem;
}

// ================================================================
// 静态辅助：插入组件到容器
// ================================================================

static bool insertIntoContainer(QQuickItem* item, QQuickItem* container,
                                const std::string& position) {
    if (!item || !container) {
        return false;
    }

    item->setParentItem(container);

    if (position == "before") {
        QList<QQuickItem*> children = container->childItems();
        if (!children.isEmpty()) {
            item->stackBefore(children.first());
        }
    }

    LOG_INFO("Component inserted into container");
    return true;
}

// ================================================================
// 加载 Component
// ================================================================

bool PluginLoader::loadComponent(const plugin::InitListEntry& entry) {
    if (entry.type != plugin::ModificationType::EXTEND) {
        LOG_WARN("loadComponent called with non-EXTEND entry");
        return false;
    }

    if (!engine_) {
        LOG_ERROR("QML engine not set");
        return false;
    }

    QString file_path = QString::fromStdString(entry.component_file);
    QObject* obj = loadQmlFile(file_path);
    if (!obj) {
        return false;
    }

    auto* item = qobject_cast<QQuickItem*>(obj);
    if (!item) {
        LOG_WARN("Loaded QML is not a QQuickItem: " + file_path.toStdString());
        recordComponent(entry.plugin_id, entry.container_id, obj, false, entry.component_file);
        return true;
    }

    QQuickItem* container = findContainer(entry.container_id);
    if (!container) {
        LOG_WARN("Container not found: " + entry.container_id);
        recordComponent(entry.plugin_id, entry.container_id, obj, false, entry.component_file);
        return true;
    }

    insertIntoContainer(item, container, entry.position);
    recordComponent(entry.plugin_id, entry.container_id, obj, false, entry.component_file);

    LOG_INFO("Component loaded: " + entry.plugin_id + " -> " + entry.container_id);
    return true;
}

// ================================================================
// 从初始化列表加载
// ================================================================

bool PluginLoader::loadFromInitList(const std::string& list_json) {
    if (!engine_) {
        LOG_ERROR("QML engine not set");
        return false;
    }

    LOG_INFO("Processing INIT_LIST...");

    auto list = plugin::initListFromJson(list_json);
    if (!list.has_value()) {
        LOG_ERROR("Failed to parse INIT_LIST");
        return false;
    }

    for (const auto& entry : list->entries) {
        if (entry.type == plugin::ModificationType::REPLACE) {
            LOG_INFO("REPLACE: target=" + entry.target_file +
                     ", winner=" + entry.winner_plugin_id);

            if (entry.target_file.find("main.qml") != std::string::npos ||
                entry.target_file.find("framework") != std::string::npos) {
                loadFramework(entry.winner_path);
            } else {
                loadQmlFile(QString::fromStdString(entry.winner_path));
            }

        } else if (entry.type == plugin::ModificationType::EXTEND) {
            LOG_INFO("EXTEND: container=" + entry.container_id +
                     ", plugin=" + entry.plugin_id +
                     ", component=" + entry.component_file);
            loadComponent(entry);
        }
    }

    LOG_INFO("INIT_LIST processing complete");
    return true;
}

// ================================================================
// 记录组件
// ================================================================

void PluginLoader::recordComponent(const std::string& plugin_id,
                                   const std::string& container_id,
                                   QObject* obj,
                                   bool is_framework,
                                   const std::string& source_file) {
    std::lock_guard<std::mutex> lock(mutex_);

    LoadedComponent comp;
    comp.plugin_id = plugin_id;
    comp.container_id = container_id;
    comp.component = nullptr;
    comp.item = qobject_cast<QQuickItem*>(obj);
    comp.loader_obj = obj;
    comp.is_framework = is_framework;
    comp.source_file = source_file;

    loaded_components_[plugin_id] = comp;
}

// ================================================================
// 查询接口（const）
// ================================================================

std::optional<LoadedComponent> PluginLoader::getComponent(const std::string& plugin_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = loaded_components_.find(plugin_id);
    if (it == loaded_components_.end()) {
        return std::nullopt;
    }
    return it->second;
}

// ================================================================
// 清理
// ================================================================

void PluginLoader::clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& pair : loaded_components_) {
        if (pair.second.loader_obj) {
            pair.second.loader_obj->deleteLater();
        }
    }
    loaded_components_.clear();

    LOG_INFO("Cleared non-framework components");
}

void PluginLoader::reset() {
    clear();

    if (framework_obj_) {
        framework_obj_->deleteLater();
        framework_obj_ = nullptr;
    }
    framework_loaded_ = false;

    LOG_INFO("PluginLoader reset");
}

} // namespace dream_machine::gui