// src/launcher/plugin_manager.cpp
#include "plugin_manager.h"

#include "logger.h"
#include "pipe.h"
#include "constants.h"
#include "messages.h"   // 新增：提供 buildMessage 和 msg_types

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;

namespace dream_machine::launcher {

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
// 构造函数 / 析构函数
// ================================================================

PluginManager::PluginManager()
    : system_plugins_dir_("plugins/system")
    , user_plugins_dir_("plugins/user")
    , status_file_path_("plugins/plugin_manifest.json")
    , integrity_file_path_("plugins/system/.integrity") {
}

PluginManager::~PluginManager() = default;

// ================================================================
// 静态辅助：计算文件哈希
// ================================================================

static std::string computeFileHash(const fs::path& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    std::hash<std::string> hasher;
    size_t hash_val = hasher(content);

    std::stringstream hex_stream;
    hex_stream << std::hex << std::setw(16) << std::setfill('0') << hash_val;
    return hex_stream.str();
}

// ================================================================
// 静态辅助：递归遍历目录
// ================================================================

static std::vector<fs::path> collectFiles(const fs::path& dir) {
    std::vector<fs::path> files;
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        return files;
    }

    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (fs::is_regular_file(entry.path())) {
            files.push_back(entry.path());
        }
    }
    return files;
}

// ================================================================
// 静态辅助：解析 manifest.json
// ================================================================

static std::optional<plugin::PluginManifest> parseManifestFile(const fs::path& manifest_path) {
    if (!fs::exists(manifest_path)) {
        LOG_ERROR("Manifest not found: " + manifest_path.string());
        return std::nullopt;
    }

    std::ifstream file(manifest_path);
    if (!file.is_open()) {
        LOG_ERROR("Failed to open manifest: " + manifest_path.string());
        return std::nullopt;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json_content = buffer.str();

    auto manifest = plugin::manifestFromJson(json_content);
    if (!manifest.has_value()) {
        LOG_ERROR("Failed to parse manifest: " + manifest_path.string());
        return std::nullopt;
    }

    return manifest;
}

// ================================================================
// 静态辅助：排序（序列号从大到小）
// ================================================================

static bool sortBySequenceDesc(const plugin::PluginManifest& a,
                               const plugin::PluginManifest& b) {
    return a.sequence > b.sequence;
}

// ================================================================
// 系统插件完整性校验（含备份恢复）
// ================================================================

bool PluginManager::generateIntegrityFile(const fs::path& dir_path,
                                          std::vector<PluginFileInfo>& out_files) {
    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
        LOG_ERROR("Directory not found for integrity generation: " + dir_path.string());
        return false;
    }

    auto files = collectFiles(dir_path);
    QJsonArray filesArray;

    for (const auto& file_path : files) {
        std::string relative_path = fs::relative(file_path, dir_path).string();
        std::string hash = computeFileHash(file_path);

        PluginFileInfo info;
        info.path = relative_path;
        info.hash = hash;
        out_files.push_back(info);

        QJsonObject fileObj;
        fileObj["path"] = toQString(relative_path);
        fileObj["hash"] = toQString(hash);
        filesArray.append(fileObj);
    }

    QJsonObject root;
    root["version"] = "1.0";
    root["generated_at"] = QString::number(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    root["files"] = filesArray;

    QJsonDocument doc(root);
    std::string json_str = doc.toJson(QJsonDocument::Indented).toStdString();

    fs::path integrity_path = dir_path / ".integrity";
    std::ofstream file(integrity_path);
    if (!file.is_open()) {
        LOG_ERROR("Failed to write integrity file: " + integrity_path.string());
        return false;
    }
    file << json_str;
    file.close();

    LOG_INFO("Integrity file generated: " + integrity_path.string() +
             " (" + std::to_string(out_files.size()) + " files)");
    return true;
}

bool PluginManager::loadIntegrityFile(const fs::path& dir_path,
                                      std::vector<PluginFileInfo>& out_files) {
    fs::path integrity_path = dir_path / ".integrity";
    if (!fs::exists(integrity_path)) {
        LOG_WARN("Integrity file not found: " + integrity_path.string());
        return false;
    }

    std::ifstream file(integrity_path);
    if (!file.is_open()) {
        LOG_ERROR("Failed to open integrity file: " + integrity_path.string());
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json_content = buffer.str();

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(
        QString::fromStdString(json_content).toUtf8(), &error
    );
    if (error.error != QJsonParseError::NoError) {
        LOG_ERROR("Failed to parse integrity file: " + integrity_path.string());
        return false;
    }

    if (!doc.isObject()) {
        LOG_ERROR("Integrity file is not a JSON object");
        return false;
    }

    QJsonObject root = doc.object();
    if (!root.contains("files") || !root["files"].isArray()) {
        LOG_ERROR("Integrity file missing 'files' array");
        return false;
    }

    QJsonArray filesArray = root["files"].toArray();
    for (const auto& val : filesArray) {
        if (!val.isObject()) continue;
        QJsonObject fileObj = val.toObject();

        PluginFileInfo info;
        info.path = toStdString(fileObj["path"].toString());
        info.hash = toStdString(fileObj["hash"].toString());
        out_files.push_back(info);
    }

    LOG_INFO("Integrity file loaded: " + integrity_path.string() +
             " (" + std::to_string(out_files.size()) + " files)");
    return true;
}

bool PluginManager::restoreSystemPluginsFromBackup() const {
    fs::path backup_dir = fs::path("data/backup/system_default");
    if (!fs::exists(backup_dir) || !fs::is_directory(backup_dir)) {
        LOG_ERROR("System plugin backup not found: " + backup_dir.string());
        return false;
    }

    fs::path target_dir = system_plugins_dir_;
    std::error_code ec;

    if (fs::exists(target_dir)) {
        fs::remove_all(target_dir, ec);
        if (ec) {
            LOG_ERROR("Failed to remove existing system plugins: " + ec.message());
            return false;
        }
    }

    fs::copy(backup_dir, target_dir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        LOG_ERROR("Failed to restore system plugins from backup: " + ec.message());
        return false;
    }

    LOG_INFO("System plugins restored from backup");
    return true;
}

bool PluginManager::hasSystemPluginBackup() {
    fs::path backup_dir = fs::path("data/backup/system_default");
    return fs::exists(backup_dir) && fs::is_directory(backup_dir) &&
           fs::exists(backup_dir / "manifest.json");
}

void PluginManager::makeSystemPluginsReadOnlyAndHidden() const {
#ifdef _WIN32
    fs::path system_dir = system_plugins_dir_;
    if (!fs::exists(system_dir)) {
        LOG_WARN("System plugins directory does not exist, cannot set attributes");
        return;
    }

    std::wstring path = system_dir.wstring();
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        LOG_WARN("GetFileAttributes failed for: " + system_dir.string() +
                 ", error: " + std::to_string(GetLastError()));
        return;
    }

    DWORD new_attrs = attrs | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN;
    if (SetFileAttributesW(path.c_str(), new_attrs) == 0) {
        LOG_WARN("SetFileAttributes failed for: " + system_dir.string() +
                 ", error: " + std::to_string(GetLastError()));
    } else {
        LOG_INFO("System plugins directory set to read-only + hidden: " + system_dir.string());
    }
#else
    LOG_INFO("Read-only + hidden not supported on this platform");
#endif
}

bool PluginManager::verifySystemPlugins() {
    if (!fs::exists(system_plugins_dir_) || !fs::is_directory(system_plugins_dir_)) {
        LOG_ERROR("System plugins directory not found: " + system_plugins_dir_.string());
        if (restoreSystemPluginsFromBackup()) {
            LOG_INFO("System plugins restored from backup, re-verifying...");
            return verifySystemPlugins();
        }
        return false;
    }

    std::vector<PluginFileInfo> expected_files;
    if (!loadIntegrityFile(system_plugins_dir_, expected_files)) {
        LOG_WARN("No integrity file found, generating one...");
        expected_files.clear();
        if (!generateIntegrityFile(system_plugins_dir_, expected_files)) {
            LOG_ERROR("Failed to generate integrity file");
            if (restoreSystemPluginsFromBackup()) {
                if (generateIntegrityFile(system_plugins_dir_, expected_files)) {
                    LOG_INFO("Integrity file regenerated after restore");
                    return true;
                }
            }
            return false;
        }
        return true;
    }

    bool all_valid = true;
    for (const auto& expected : expected_files) {
        fs::path file_path = system_plugins_dir_ / expected.path;
        if (!fs::exists(file_path)) {
            LOG_ERROR("Missing system plugin file: " + expected.path);
            all_valid = false;
            continue;
        }

        std::string actual_hash = computeFileHash(file_path);
        if (actual_hash != expected.hash) {
            LOG_ERROR("Hash mismatch for system plugin file: " + expected.path);
            all_valid = false;
        }
    }

    if (!all_valid) {
        LOG_WARN("System plugin integrity check failed, attempting restore...");
        if (restoreSystemPluginsFromBackup()) {
            std::vector<PluginFileInfo> restored_files;
            if (loadIntegrityFile(system_plugins_dir_, restored_files)) {
                LOG_INFO("System plugins restored successfully");
                if (generateIntegrityFile(system_plugins_dir_, restored_files)) {
                    LOG_INFO("Integrity file regenerated after restore");
                    return true;
                }
            }
        }
        return false;
    }

    LOG_INFO("System plugin integrity check passed");
    return true;
}

// ================================================================
// 扫描插件
// ================================================================

bool PluginManager::loadPluginFromDirectory(const fs::path& dir_path, bool system) {
    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
        return false;
    }

    for (const auto& entry : fs::directory_iterator(dir_path)) {
        if (!fs::is_directory(entry.path())) {
            continue;
        }

        fs::path manifest_path = entry.path() / "manifest.json";
        auto manifest = parseManifestFile(manifest_path);
        if (!manifest.has_value()) {
            LOG_WARN("Failed to load plugin from " + entry.path().string() +
                     ", skipping");
            continue;
        }

        manifest->system = system;

        // 系统插件强制序列号为 0
        if (manifest->system) {
            manifest->sequence = 0;
        }

        auto status_it = plugin_status_.find(manifest->id);
        if (status_it != plugin_status_.end()) {
            manifest->enabled = status_it->second.enabled && !status_it->second.disabled_by_error;
        } else {
            manifest->enabled = true;
        }

        loaded_manifests_[manifest->id] = *manifest;
        LOG_INFO("Loaded plugin: " + manifest->id + " (" +
                 plugin::pluginTypeToString(manifest->type) + ", " +
                 (system ? "system" : "user") + ")" +
                 (manifest->system ? " [sequence=0]" : ""));
    }

    return true;
}

bool PluginManager::scanPlugins() {
    LOG_INFO("Scanning plugins...");

    if (!loadPluginStatus()) {
        LOG_WARN("Failed to load plugin status, using defaults");
    }

    // 验证系统插件完整性（含恢复）
    if (!verifySystemPlugins()) {
        LOG_WARN("System plugin integrity check failed, continuing anyway");
    }

    // 加载系统插件
    if (fs::exists(system_plugins_dir_) && fs::is_directory(system_plugins_dir_)) {
        loadPluginFromDirectory(system_plugins_dir_, true);
    } else {
        LOG_WARN("System plugins directory not found: " + system_plugins_dir_.string());
    }

    // 设置系统插件目录为只读+隐藏
    makeSystemPluginsReadOnlyAndHidden();

    // 加载用户插件
    if (fs::exists(user_plugins_dir_) && fs::is_directory(user_plugins_dir_)) {
        loadPluginFromDirectory(user_plugins_dir_, false);
    } else {
        LOG_INFO("User plugins directory not found, creating...");
        fs::create_directories(user_plugins_dir_);
    }

    LOG_INFO("Plugin scan complete: " + std::to_string(loaded_manifests_.size()) +
             " plugins loaded");
    return true;
}

// ================================================================
// 生成初始化列表
// ================================================================

bool PluginManager::isTargetOverridden(const std::string& target_file,
                                       const std::vector<plugin::InitListEntry>& existing_entries) {
    return std::any_of(existing_entries.begin(), existing_entries.end(),
        [&](const plugin::InitListEntry& entry) {
            return entry.type == plugin::ModificationType::REPLACE &&
                   entry.target_file == target_file;
        });
}

plugin::InitList PluginManager::generateInitList() const {
    plugin::InitList list;
    list.version = 1;
    list.generated_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    std::vector<plugin::PluginManifest> enabled_plugins;
    for (const auto& pair : loaded_manifests_) {
        if (pair.second.enabled) {
            enabled_plugins.push_back(pair.second);
        }
    }

    std::sort(enabled_plugins.begin(), enabled_plugins.end(), sortBySequenceDesc);

    std::unordered_map<std::string, bool> replaced_targets;

    // 1. 计算 REPLACE 关系
    for (const auto& manifest : enabled_plugins) {
        if (manifest.modification != plugin::ModificationType::REPLACE) {
            continue;
        }

        if (!manifest.target.has_value() || manifest.target->file.empty()) {
            continue;
        }

        std::string target_file = manifest.target->file;

        if (replaced_targets.find(target_file) != replaced_targets.end()) {
            continue;
        }

        replaced_targets[target_file] = true;

        std::vector<std::string> disabled_plugins;
        for (const auto& other : enabled_plugins) {
            if (other.id == manifest.id) continue;
            if (other.modification != plugin::ModificationType::REPLACE) continue;
            if (!other.target.has_value()) continue;
            if (other.target->file != target_file) continue;
            disabled_plugins.push_back(other.id);
        }

        plugin::InitListEntry entry;
        entry.type = plugin::ModificationType::REPLACE;
        entry.target_file = target_file;
        entry.winner_plugin_id = manifest.id;
        entry.winner_sequence = manifest.sequence;
        std::string base_dir = manifest.system ? "plugins/system/" : "plugins/user/";
        entry.winner_path = base_dir + manifest.id + "/" + target_file;
        entry.disabled_plugins = disabled_plugins;

        list.entries.push_back(std::move(entry));
    }

    // 2. 计算 EXTEND 关系
    for (const auto& manifest : enabled_plugins) {
        if (manifest.modification != plugin::ModificationType::EXTEND) {
            continue;
        }

        if (!manifest.target.has_value() || manifest.target->container.empty()) {
            continue;
        }

        if (manifest.type != plugin::PluginType::COMPONENT) {
            continue;
        }

        plugin::InitListEntry entry;
        entry.type = plugin::ModificationType::EXTEND;
        entry.container_id = manifest.target->container;
        entry.plugin_id = manifest.id;
        entry.position = manifest.target->position.empty() ? "after" : manifest.target->position;

        if (!manifest.entry.components.empty()) {
            std::string base_dir = manifest.system ? "plugins/system/" : "plugins/user/";
            entry.component_file = base_dir + manifest.id + "/" + manifest.entry.components[0];
        } else {
            LOG_WARN("Plugin " + manifest.id + " has EXTEND but no components");
            continue;
        }

        entry.enabled = manifest.enabled;

        if (manifest.flow_rules.has_value()) {
            entry.rule_file = *manifest.flow_rules;
        }

        list.entries.push_back(std::move(entry));
    }

    LOG_INFO("Generated init list: " + std::to_string(list.entries.size()) +
             " entries (" +
             std::to_string(replaced_targets.size()) + " REPLACE, " +
             std::to_string(list.entries.size() - replaced_targets.size()) + " EXTEND)");

    return list;
}

// ================================================================
// 分发初始化列表
// ================================================================

bool PluginManager::sendInitListToProcess(NamedPipe& pipe,
                                          const plugin::InitList& list,
                                          const std::string& process_name,
                                          int timeout_ms) {
    if (!pipe.isValid() || !pipe.isConnected()) {
        LOG_WARN("Cannot send INIT_LIST to " + process_name + ": pipe not connected");
        return false;
    }

    std::string list_json = plugin::initListToJson(list);
    // 使用 buildMessage 确保 payload 是 JSON 字符串
    std::string message = dream_machine::buildMessage(dream_machine::msg_types::INIT_LIST, "", list_json);

    if (pipe.writeLine(message) != PipeResult::PIPE_OK) {
        LOG_ERROR("Failed to send INIT_LIST to " + process_name);
        return false;
    }

    LOG_INFO("Sent INIT_LIST to " + process_name);

    auto start_time = std::chrono::steady_clock::now();
    while (true) {
        if (pipe.isBroken()) {
            LOG_WARN(process_name + " pipe broken while waiting for ACK");
            return false;
        }

        DWORD bytes_available = 0;
        PipeResult peek = pipe.peekAvailable(bytes_available);
        if (peek == PipeResult::PIPE_OK && bytes_available > 0) {
            std::string response;
            PipeResult read_result = pipe.readLine(response, 100);
            if (read_result == PipeResult::PIPE_OK) {
                if (response.find("INIT_LIST_ACK") != std::string::npos) {
                    LOG_INFO(process_name + " sent INIT_LIST_ACK");
                    return true;
                }
            } else if (read_result == PipeResult::PIPE_BROKEN) {
                return false;
            }
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time
        );
        if (elapsed.count() > timeout_ms) {
            LOG_WARN(process_name + " INIT_LIST_ACK timeout after " +
                     std::to_string(timeout_ms) + "ms");
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool PluginManager::distributeInitList(NamedPipe& gui_pipe,
                                       NamedPipe& executor_pipe,
                                       NamedPipe& monitor_pipe,
                                       const plugin::InitList& list) {
    LOG_INFO("Distributing init list to all processes...");

    bool gui_ok = sendInitListToProcess(gui_pipe, list, "gui");
    bool executor_ok = sendInitListToProcess(executor_pipe, list, "executor");
    bool monitor_ok = sendInitListToProcess(monitor_pipe, list, "monitor");

    if (gui_ok && executor_ok && monitor_ok) {
        LOG_INFO("All processes acknowledged INIT_LIST");
        return true;
    } else {
        LOG_WARN("Some processes failed to acknowledge INIT_LIST: " +
                 std::string(gui_ok ? "gui OK " : "gui FAIL ") +
                 std::string(executor_ok ? "executor OK " : "executor FAIL ") +
                 std::string(monitor_ok ? "monitor OK" : "monitor FAIL"));
        return false;
    }
}

// ================================================================
// 插件状态管理
// ================================================================

bool PluginManager::loadPluginStatus() {
    if (!fs::exists(status_file_path_)) {
        LOG_INFO("Plugin status file not found, using defaults");
        return true;
    }

    std::ifstream file(status_file_path_);
    if (!file.is_open()) {
        LOG_ERROR("Failed to open plugin status file: " + status_file_path_.string());
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json_content = buffer.str();

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(
        QString::fromStdString(json_content).toUtf8(), &error
    );
    if (error.error != QJsonParseError::NoError) {
        LOG_ERROR("Failed to parse plugin status file: " + status_file_path_.string());
        return false;
    }

    if (!doc.isArray()) {
        LOG_ERROR("Plugin status file is not a JSON array");
        return false;
    }

    QJsonArray array = doc.array();
    for (const auto& val : array) {
        if (!val.isObject()) continue;
        QJsonObject obj = val.toObject();

        plugin::PluginStatus status;
        status.plugin_id = toStdString(obj["plugin_id"].toString());
        status.enabled = obj["enabled"].toBool(false);
        status.disabled_by_error = obj["disabled_by_error"].toBool(false);
        status.error_reason = toStdString(obj["error_reason"].toString());
        status.error_timestamp = static_cast<uint64_t>(obj["error_timestamp"].toInteger(0));

        if (!status.plugin_id.empty()) {
            plugin_status_[status.plugin_id] = status;
        }
    }

    LOG_INFO("Loaded plugin status: " + std::to_string(plugin_status_.size()) + " entries");
    return true;
}

bool PluginManager::savePluginStatus() const {
    QJsonArray array;
    for (const auto& pair : plugin_status_) {
        QJsonObject obj;
        obj["plugin_id"] = toQString(pair.second.plugin_id);
        obj["enabled"] = pair.second.enabled;
        obj["disabled_by_error"] = pair.second.disabled_by_error;
        obj["error_reason"] = toQString(pair.second.error_reason);
        obj["error_timestamp"] = static_cast<qint64>(pair.second.error_timestamp);
        array.append(obj);
    }

    QJsonDocument doc(array);
    std::string json_str = doc.toJson(QJsonDocument::Indented).toStdString();

    fs::create_directories(fs::path(status_file_path_).parent_path());

    std::ofstream file(status_file_path_);
    if (!file.is_open()) {
        LOG_ERROR("Failed to save plugin status file: " + status_file_path_.string());
        return false;
    }
    file << json_str;
    file.close();

    LOG_INFO("Saved plugin status: " + std::to_string(plugin_status_.size()) + " entries");
    return true;
}

bool PluginManager::setPluginEnabled(const std::string& plugin_id, bool enabled) {
    auto it = plugin_status_.find(plugin_id);
    if (it == plugin_status_.end()) {
        plugin::PluginStatus status;
        status.plugin_id = plugin_id;
        status.enabled = enabled;
        status.disabled_by_error = false;
        plugin_status_[plugin_id] = status;
    } else {
        it->second.enabled = enabled;
        if (enabled) {
            it->second.disabled_by_error = false;
            it->second.error_reason.clear();
        }
    }

    auto manifest_it = loaded_manifests_.find(plugin_id);
    if (manifest_it != loaded_manifests_.end()) {
        manifest_it->second.enabled = enabled && !plugin_status_[plugin_id].disabled_by_error;
    }

    return savePluginStatus();
}

std::optional<plugin::PluginStatus> PluginManager::getPluginStatus(const std::string& plugin_id) const {
    auto it = plugin_status_.find(plugin_id);
    if (it == plugin_status_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<plugin::PluginStatus> PluginManager::getAllPluginStatus() const {
    std::vector<plugin::PluginStatus> result;
    result.reserve(plugin_status_.size());
    for (const auto& pair : plugin_status_) {
        result.push_back(pair.second);
    }
    return result;
}

bool PluginManager::disablePluginOnError(const std::string& plugin_id, const std::string& reason) {
    auto it = plugin_status_.find(plugin_id);
    if (it == plugin_status_.end()) {
        plugin::PluginStatus status;
        status.plugin_id = plugin_id;
        status.enabled = false;
        status.disabled_by_error = true;
        status.error_reason = reason;
        status.error_timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        plugin_status_[plugin_id] = status;
    } else {
        it->second.enabled = false;
        it->second.disabled_by_error = true;
        it->second.error_reason = reason;
        it->second.error_timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    auto manifest_it = loaded_manifests_.find(plugin_id);
    if (manifest_it != loaded_manifests_.end()) {
        manifest_it->second.enabled = false;
    }

    LOG_WARN("Plugin " + plugin_id + " disabled due to error: " + reason);
    return savePluginStatus();
}

// ================================================================
// 查询接口
// ================================================================

std::vector<std::string> PluginManager::getSystemPluginIds() const {
    std::vector<std::string> result;
    for (const auto& pair : loaded_manifests_) {
        if (pair.second.system) {
            result.push_back(pair.first);
        }
    }
    return result;
}

std::vector<std::string> PluginManager::getUserPluginIds() const {
    std::vector<std::string> result;
    for (const auto& pair : loaded_manifests_) {
        if (!pair.second.system) {
            result.push_back(pair.first);
        }
    }
    return result;
}

bool PluginManager::pluginExists(const std::string& plugin_id) const {
    return loaded_manifests_.find(plugin_id) != loaded_manifests_.end();
}

// ================================================================
// 导入/删除插件
// ================================================================

bool PluginManager::importPlugin(const std::string& package_path, std::string& out_plugin_id) {
    LOG_INFO("Importing plugin from: " + package_path);

    // TODO: 实现插件包解压
    LOG_WARN("Plugin import not fully implemented yet");
    return false;
}

bool PluginManager::deletePlugin(const std::string& plugin_id) {
    LOG_INFO("Deleting plugin: " + plugin_id);

    auto it = loaded_manifests_.find(plugin_id);
    if (it == loaded_manifests_.end()) {
        LOG_ERROR("Plugin not found: " + plugin_id);
        return false;
    }
    if (it->second.system) {
        LOG_ERROR("Cannot delete system plugin: " + plugin_id);
        return false;
    }

    fs::path plugin_dir = user_plugins_dir_ / plugin_id;
    if (fs::exists(plugin_dir)) {
        std::error_code ec;
        fs::remove_all(plugin_dir, ec);
        if (ec) {
            LOG_ERROR("Failed to delete plugin directory: " + plugin_dir.string());
            return false;
        }
    }

    loaded_manifests_.erase(it);
    plugin_status_.erase(plugin_id);
    if (!savePluginStatus()) {
        LOG_WARN("Failed to save plugin status after deletion");
    }

    LOG_INFO("Plugin deleted: " + plugin_id);
    return true;
}

} // namespace dream_machine::launcher