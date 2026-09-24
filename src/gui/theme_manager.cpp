// src/gui/theme_manager.cpp
#include "theme_manager.h"

#include "logger.h"
#include "common_utils.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <cstdlib>

namespace dream_machine::gui {

// ================================================================
// 匿名命名空间：内部辅助
// ================================================================
namespace {

// 系统模板的相对路径（生产模式，相对 exe 目录）
constexpr const char* SYS_TEMPLATE_RELATIVE =
    "plugins/system/dream_machine_default/config/global_params.json";

// 开发模式下的系统模板相对项目根的路径
constexpr const char* SYS_TEMPLATE_DEV_RELATIVE =
    "src/default_plugin/config/global_params.json";

// 用户偏好的相对路径（相对 exe 目录）
constexpr const char* USER_PREFS_RELATIVE = "data/theme_preferences.json";

// 兜底模式列表（系统模板不可读时使用）
const QStringList FALLBACK_MODES = {"light", "dark"};

// 判断是否处于开发模式（读取 DM_DEV_MODE 环境变量）
bool isDevMode() {
    const char* dev_mode = std::getenv("DM_DEV_MODE");
    return dev_mode && std::string(dev_mode) == "1";
}

// 读取 JSON 文件 → QVariantMap
// 失败返回空 map
QVariantMap readJsonFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QByteArray data = file.readAll();
    file.close();

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        LOG_WARN("ThemeManager: failed to parse JSON: " + path.toStdString() +
                 " (" + error.errorString().toStdString() + ")");
        return {};
    }
    return doc.object().toVariantMap();
}

} // namespace

// ================================================================
// 构造函数 / 析构函数
// ================================================================

ThemeManager::ThemeManager(QObject* parent)
    : QObject(parent) {
    loadFromFiles();
}

ThemeManager::~ThemeManager() = default;

// ================================================================
// 公开接口
// ================================================================

QString ThemeManager::currentMode() const {
    return current_mode_;
}

bool ThemeManager::setMode(const QString& mode) {
    // ----- 1. 校验 mode -----
    if (!available_modes_.contains(mode)) {
        LOG_WARN("ThemeManager::setMode: unknown mode '" + mode.toStdString() +
                 "' (available: " + available_modes_.join(", ").toStdString() + ")");
        return false;
    }

    // ----- 2. 幂等 -----
    if (mode == current_mode_) {
        LOG_INFO("ThemeManager::setMode: mode '" + mode.toStdString() +
                 "' already active, no-op");
        return true;
    }

    // ----- 3. 从磁盘重新读用户偏好（确保最新；用户可能手动改过文件） -----
    QString user_path = QString::fromStdString(
        common::pathFromRoot(USER_PREFS_RELATIVE));
    QVariantMap fresh_prefs = readJsonFile(user_path);

    // ----- 4. 更新 theme_mode（保留其他字段） -----
    fresh_prefs["theme_mode"] = mode;

    // ----- 5. 写回磁盘 -----
    QString parent_dir = QFileInfo(user_path).absolutePath();
    QDir dir;
    if (!dir.mkpath(parent_dir)) {
        LOG_ERROR("ThemeManager::setMode: failed to create directory: " +
                  parent_dir.toStdString());
        return false;
    }

    QJsonObject json_obj = QJsonObject::fromVariantMap(fresh_prefs);
    QJsonDocument doc(json_obj);
    QByteArray data = doc.toJson(QJsonDocument::Indented);

    QFile file(user_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        LOG_ERROR("ThemeManager::setMode: failed to open for write: " +
                  user_path.toStdString());
        return false;
    }
    if (file.write(data) != data.size()) {
        LOG_ERROR("ThemeManager::setMode: partial write to: " +
                  user_path.toStdString());
        file.close();
        return false;
    }
    file.close();

    LOG_INFO("ThemeManager::setMode: saved theme_mode='" + mode.toStdString() +
             "' to " + user_path.toStdString());

    // ----- 6. 更新内存状态 + 通知 -----
    current_mode_ = mode;
    user_prefs_ = fresh_prefs;
    emit modeChanged();

    return true;
}

QStringList ThemeManager::availableModes() const {
    return available_modes_;
}

// ================================================================
// 私有：加载流程
// ================================================================

void ThemeManager::loadFromFiles() {
    // ----- 1. 读系统模板 -----
    QVariantMap sys_template = readSystemTemplate();

    // ----- 2. 提取可用模式 + 默认模式 -----
    available_modes_ = extractModesFromTemplate(sys_template);
    if (available_modes_.isEmpty()) {
        available_modes_ = FALLBACK_MODES;
        LOG_WARN("ThemeManager: no modes in system template, using fallback");
    }

    QString default_mode = extractDefaultMode(sys_template);
    if (default_mode.isEmpty() || !available_modes_.contains(default_mode)) {
        default_mode = available_modes_.first();
    }

    // ----- 3. 读用户偏好 -----
    QString user_path = QString::fromStdString(
        common::pathFromRoot(USER_PREFS_RELATIVE));
    user_prefs_ = readJsonFile(user_path);

    // ----- 4. 决定当前模式 -----
    QString user_mode = user_prefs_.value("theme_mode").toString();
    if (!user_mode.isEmpty() && available_modes_.contains(user_mode)) {
        current_mode_ = user_mode;
    } else {
        current_mode_ = default_mode;
        if (!user_mode.isEmpty()) {
            LOG_WARN("ThemeManager: user mode '" + user_mode.toStdString() +
                     "' not in available modes, using default '" +
                     default_mode.toStdString() + "'");
        }
    }

    LOG_INFO("ThemeManager initialized: currentMode=" + current_mode_.toStdString() +
             ", available=[" + available_modes_.join(", ").toStdString() + "]");
}

QVariantMap ThemeManager::readSystemTemplate() const {
    // 生产路径
    QString sys_path = QString::fromStdString(
        common::pathFromRoot(SYS_TEMPLATE_RELATIVE));
    QVariantMap sys_template = readJsonFile(sys_path);
    if (!sys_template.isEmpty()) {
        return sys_template;
    }

    // dev 路径
    if (isDevMode()) {
        QString project_root = QDir::currentPath();
        QDir proj_dir(project_root);
        if (proj_dir.dirName() == "bin") {
            proj_dir.cdUp();
            proj_dir.cdUp();
        }
        QString dev_path = proj_dir.filePath(QString::fromUtf8(SYS_TEMPLATE_DEV_RELATIVE));
        sys_template = readJsonFile(dev_path);
        if (!sys_template.isEmpty()) {
            return sys_template;
        }
    }

    return {};
}

QStringList ThemeManager::extractModesFromTemplate(const QVariantMap& sys_template) const {
    QStringList result;

    QVariantMap theme = sys_template.value("theme").toMap();
    if (theme.isEmpty()) {
        return result;
    }
    QVariantMap modes = theme.value("modes").toMap();
    for (auto it = modes.begin(); it != modes.end(); ++it) {
        result.push_back(it.key());
    }
    // 稳定排序：light 在前，dark 在后，其他按字母序
    result.sort();
    return result;
}

QString ThemeManager::extractDefaultMode(const QVariantMap& sys_template) const {
    QVariantMap theme = sys_template.value("theme").toMap();
    return theme.value("default_mode").toString();
}

} // namespace dream_machine::gui