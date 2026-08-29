// platform/dm_base/plugin_types.h
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstdint>


namespace dream_machine::plugin {

// ================================================================
// 插件类型枚举
// ================================================================
enum class PluginType : uint8_t {
    FRAMEWORK,          // 总体框架（主窗口布局），需重启 launcher
    COMPONENT,          // 框架部分列表添加控件，热加载
    CARD,               // 弹出卡片文件，热加载
    SHELF_EXTENSION     // 历史陈列架扩展，热加载
};

// ================================================================
// 插件修改类型
// ================================================================
enum class ModificationType : uint8_t {
    REPLACE,            // 覆盖（替换目标文件）
    EXTEND              // 拓展（插入到容器）
};

// ================================================================
// 插件覆盖策略
// ================================================================
struct OverridePolicy {
    bool allow_function_change = true;
    bool allow_style_change = true;
    bool allow_position_change = true;
};

// ================================================================
// 插件清单（manifest.json）
// ================================================================
struct PluginManifest {
    std::string id;                     // 插件唯一标识
    std::string name;                   // 显示名称
    std::string version;                // 版本号
    bool system = false;                // 是否系统插件
    PluginType type = PluginType::COMPONENT;
    ModificationType modification = ModificationType::EXTEND;

    // 覆盖目标
    struct Target {
        std::string file;               // 覆盖时：目标文件路径
        std::string container;          // 拓展时：目标容器 ID
        std::string position;           // 拓展时：插入位置（before/after/inside）
    };
    std::optional<Target> target;

    // 入口文件
    struct Entry {
        std::string framework;          // framework 类型：QML 主文件路径
        std::vector<std::string> components;   // component 类型：QML 文件列表
        std::vector<std::string> cards;         // card 类型：QML 文件列表
        std::optional<std::string> global_params; // 全局参数文件路径
        std::vector<std::string> scripts;       // 脚本文件列表
    };
    Entry entry;

    // 触发脚本映射（事件名 -> 脚本路径）
    std::unordered_map<std::string, std::string> triggers;

    // 流程规则文件路径
    std::optional<std::string> flow_rules;

    bool can_delete = true;
    bool can_disable = true;
    bool overridable = true;
    OverridePolicy override_policy;

    // 序列号（默认按添加顺序，用户可拖动排序）
    uint32_t sequence = 0;

    // 启用状态（由 plugin_manifest.json 管理，非插件包内定义）
    bool enabled = false;
};

// ================================================================
// 初始化列表条目
// ================================================================
struct InitListEntry {
    ModificationType type;

    // REPLACE 字段
    std::string target_file;            // 被覆盖的目标文件路径（相对于插件根目录）
    std::string winner_plugin_id;       // 胜出插件 ID
    uint32_t winner_sequence = 0;
    std::string winner_path;            // 胜出插件的完整文件路径
    std::vector<std::string> disabled_plugins;   // 被禁用的插件列表

    // EXTEND 字段
    std::string container_id;           // 目标容器 ID
    std::string plugin_id;              // 插入插件 ID
    std::string component_file;         // 插件组件文件路径
    std::string position;               // 插入位置（before/after）
    bool enabled = true;

    // 通用字段
    std::string trigger;                // 触发事件名称（用于 flow_rule）
    std::string rule_file;              // 规则文件路径
};

// ================================================================
// 初始化列表
// ================================================================
struct InitList {
    std::vector<InitListEntry> entries;
    uint64_t generated_at = 0;          // 生成时间戳
    uint32_t version = 1;               // 列表版本号
};

// ================================================================
// 触发事件常量（16 个事件）
// ================================================================
namespace triggers {
    inline constexpr const char* ON_CORE_ENGINE_START = "on_core_engine_start";
    inline constexpr const char* ON_CORE_ENGINE_EXIT = "on_core_engine_exit";
    inline constexpr const char* ON_SESSION_ENTER = "on_session_enter";
    inline constexpr const char* ON_SESSION_EXIT = "on_session_exit";
    inline constexpr const char* ON_USER_INPUT = "on_user_input";
    inline constexpr const char* ON_DIALOG_START = "on_dialog_start";
    inline constexpr const char* ON_DIALOG_END = "on_dialog_end";
    inline constexpr const char* ON_REPLY_GENERATED = "on_reply_generated";
    inline constexpr const char* ON_TASK_START = "on_task_start";
    inline constexpr const char* ON_TASK_UPDATE = "on_task_update";
    inline constexpr const char* ON_TASK_END = "on_task_end";
    inline constexpr const char* ON_CANCELLED = "on_cancelled";
    inline constexpr const char* ON_L1_READ = "on_l1_read";
    inline constexpr const char* ON_L2_READ = "on_l2_read";
    inline constexpr const char* ON_L2_WRITE = "on_l2_write";
    inline constexpr const char* ON_L2_WRITE_COMPLETE = "on_l2_write_complete";
}

// ================================================================
// 插件状态（plugin_manifest.json）
// ================================================================
struct PluginStatus {
    std::string plugin_id;
    bool enabled = false;               // 用户启用状态
    bool disabled_by_error = false;     // 是否因异常被自动禁用
    std::string error_reason;           // 禁用原因
    uint64_t error_timestamp = 0;       // 错误时间
};

// ================================================================
// 序列化辅助函数声明（实现在 .cpp 中）
// ================================================================

// 将 PluginManifest 转为 JSON 字符串
std::string manifestToJson(const PluginManifest& manifest);

// 从 JSON 字符串解析 PluginManifest
std::optional<PluginManifest> manifestFromJson(const std::string& json);

// 将 InitList 转为 JSON 字符串
std::string initListToJson(const InitList& list);

// 从 JSON 字符串解析 InitList
std::optional<InitList> initListFromJson(const std::string& json);

// 将 PluginStatus 转为 JSON 字符串
std::string statusToJson(const PluginStatus& status);

// 从 JSON 字符串解析 PluginStatus
std::optional<PluginStatus> statusFromJson(const std::string& json);

// 将 PluginType 转为字符串
std::string pluginTypeToString(PluginType type);
PluginType stringToPluginType(const std::string& str);

// 将 ModificationType 转为字符串
std::string modTypeToString(ModificationType type);
ModificationType stringToModType(const std::string& str);

} // namespace dream_machine::plugin
