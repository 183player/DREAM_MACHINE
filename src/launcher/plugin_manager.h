// src/launcher/plugin_manager.h
#pragma once

#include "plugin_types.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <optional>

// 前向声明
namespace dream_machine {
    class NamedPipe;
}

namespace dream_machine::launcher {

// ================================================================
// 插件管理器（launcher 内部使用）
// ================================================================
class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    // 禁止拷贝
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // ============================================================
    // 初始化与扫描
    // ============================================================

    [[nodiscard]] bool scanPlugins();
    [[nodiscard]] bool verifySystemPlugins();

    // ============================================================
    // 初始化列表生成
    // ============================================================

    [[nodiscard]] plugin::InitList generateInitList() const;

    // ============================================================
    // 分发初始化列表
    // ============================================================

    [[nodiscard]] static bool sendInitListToProcess(NamedPipe& pipe,
                                                    const plugin::InitList& list,
                                                    const std::string& process_name,
                                                    int timeout_ms = 3000);

    [[nodiscard]] bool distributeInitList(NamedPipe& gui_pipe,
                                          NamedPipe& executor_pipe,
                                          NamedPipe& monitor_pipe,
                                          const plugin::InitList& list);

    // ============================================================
    // 插件状态管理
    // ============================================================

    [[nodiscard]] bool loadPluginStatus();
    [[nodiscard]] bool savePluginStatus() const;
    [[nodiscard]] bool setPluginEnabled(const std::string& plugin_id, bool enabled);
    [[nodiscard]] std::optional<plugin::PluginStatus> getPluginStatus(const std::string& plugin_id) const;
    [[nodiscard]] std::vector<plugin::PluginStatus> getAllPluginStatus() const;
    [[nodiscard]] bool disablePluginOnError(const std::string& plugin_id, const std::string& reason);

    // ============================================================
    // 查询接口
    // ============================================================

    [[nodiscard]] const std::unordered_map<std::string, plugin::PluginManifest>& getLoadedManifests() const {
        return loaded_manifests_;
    }

    [[nodiscard]] std::vector<std::string> getSystemPluginIds() const;
    [[nodiscard]] std::vector<std::string> getUserPluginIds() const;
    [[nodiscard]] bool pluginExists(const std::string& plugin_id) const;

    // ============================================================
    // 导出/导入
    // ============================================================

    [[nodiscard]] bool importPlugin(const std::string& package_path, std::string& out_plugin_id);
    [[nodiscard]] bool deletePlugin(const std::string& plugin_id);

    // ============================================================
    // 诊断工具
    // ============================================================

    // 检查系统插件备份是否存在（静态方法，不依赖实例）
    [[nodiscard]] static bool hasSystemPluginBackup();

private:
    // ============================================================
    // 内部数据结构
    // ============================================================

    struct PluginFileInfo {
        std::string path;
        std::string hash;
    };

    // 目录路径
    std::filesystem::path system_plugins_dir_;
    std::filesystem::path user_plugins_dir_;
    std::filesystem::path status_file_path_;
    std::filesystem::path integrity_file_path_;

    // 加载的 manifest
    std::unordered_map<std::string, plugin::PluginManifest> loaded_manifests_;

    // 插件状态
    std::unordered_map<std::string, plugin::PluginStatus> plugin_status_;

    // ============================================================
    // 内部辅助函数
    // ============================================================

    // 生成完整性校验文件
    static bool generateIntegrityFile(const std::filesystem::path& dir_path,
                                      std::vector<PluginFileInfo>& out_files);

    // 加载完整性校验文件
    static bool loadIntegrityFile(const std::filesystem::path& dir_path,
                                  std::vector<PluginFileInfo>& out_files);

    // 加载单个插件目录
    bool loadPluginFromDirectory(const std::filesystem::path& dir_path, bool system);

    // 检查目标是否已被覆盖
    static bool isTargetOverridden(const std::string& target_file,
                                   const std::vector<plugin::InitListEntry>& existing_entries);

    // ============================================================
    // 系统插件保护与恢复
    // ============================================================

    // 设置系统插件目录为只读+隐藏（Windows）
    void makeSystemPluginsReadOnlyAndHidden() const;

    // 从备份恢复系统插件
    bool restoreSystemPluginsFromBackup() const;
};

} // namespace dream_machine::launcher