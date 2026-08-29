// src/gui/plugin_loader.h
#pragma once

#include "plugin_types.h"
#include "messages.h"

#include <QObject>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>

#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <optional>

namespace dream_machine::gui {

// ================================================================
// 已加载组件信息
// ================================================================
struct LoadedComponent {
    std::string plugin_id;
    std::string container_id;
    QQmlComponent* component = nullptr;
    QQuickItem* item = nullptr;
    QObject* loader_obj = nullptr;
    bool is_framework = false;
    std::string source_file;
};

// ================================================================
// 插件加载器（gui 内部使用）
// ================================================================
class PluginLoader {
public:
    PluginLoader();
    ~PluginLoader();

    // 禁止拷贝
    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    // ============================================================
    // 初始化
    // ============================================================

    void setEngine(QQmlApplicationEngine* engine);

    [[nodiscard]] QQmlApplicationEngine* getEngine() const { return engine_; }

    // ============================================================
    // 加载接口
    // ============================================================

    bool loadFromInitList(const std::string& list_json);

    // 加载单个 QML 文件（const，不修改对象状态）
    QObject* loadQmlFile(const QString& file_path, QObject* parent = nullptr) const;

    bool loadFramework(const std::string& file_path);

    bool loadComponent(const plugin::InitListEntry& entry);

    // ============================================================
    // 查询接口
    // ============================================================

    [[nodiscard]] const std::unordered_map<std::string, LoadedComponent>& getLoadedComponents() const {
        return loaded_components_;
    }

    [[nodiscard]] std::optional<LoadedComponent> getComponent(const std::string& plugin_id) const;

    [[nodiscard]] bool isFrameworkLoaded() const { return framework_loaded_; }

    [[nodiscard]] QObject* getFramework() const { return framework_obj_; }

    // ============================================================
    // 清理
    // ============================================================

    void clear();

    void reset();

private:
    // ============================================================
    // 内部辅助
    // ============================================================

    [[nodiscard]] QQuickItem* findContainer(const std::string& container_id) const;

    void recordComponent(const std::string& plugin_id,
                         const std::string& container_id,
                         QObject* obj,
                         bool is_framework,
                         const std::string& source_file);

    // ============================================================
    // 成员变量（mutex_ 为 mutable 以支持 const 函数）
    // ============================================================

    QQmlApplicationEngine* engine_ = nullptr;
    std::unordered_map<std::string, LoadedComponent> loaded_components_;
    mutable std::mutex mutex_;

    bool framework_loaded_ = false;
    QObject* framework_obj_ = nullptr;
};

} // namespace dream_machine::gui