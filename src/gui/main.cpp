// src/gui/main.cpp
#include "logger.h"
#include "pipe.h"
#include "constants.h"
#include "messages.h"
#include "message_router.h"
#include "session_state_manager.h"
#include "plugin_loader.h"
#include "status_provider.h"
#include "theme_manager.h"
#include "common_utils.h"

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTimer>
#include <QUrl>
#include <QObject>
#include <QFileInfo>
#include <QFile>
#include <QString>
#include <QCoreApplication>
#include <QDir>
#include <QQuickWindow>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantMap>
#include <QPointer>

#include <string>
#include <cstdlib>
#include <tlhelp32.h>
#include <memory>
#include <atomic>

using namespace dream_machine;
using namespace dream_machine::gui;
// 注：不再 using dream_machine::common——
//     新增的 utf8ToWide / wideToUtf8 / pathFromRoot 用 common:: 前缀显式调用。

// ================================================================
// 未来重审点（依据 DREAM_MACHINE_CONCURRENCY_MODEL_BOUNDARY 专家裁决 Q4）：
//
//   1. 若 GUI 引入 QJSEngine 工作线程（插件脚本执行），需重审：
//        - Qt 对象访问必须通过 QMetaObject::invokeMethod 跨线程；
//        - Logger 的 thread_local channel 保证各线程日志隔离；
//        - g_pipe 改 std::shared_ptr<NamedPipe> 保证生命周期；
//   2. Logger 的 mutex_ 保留——即使引入线程也无需重构 Logger 本身。
//
//   当前单线程 Qt 事件循环 + 50ms QTimer 轮询模型下无需改造。
// ================================================================

// ================================================================
// 全局状态（使用 unique_ptr / QPointer 管理）
// ================================================================
static std::unique_ptr<NamedPipe> g_pipe;
static QPointer<SessionStateManager> g_sessionManager;
static QPointer<QQmlApplicationEngine> g_engine;
static std::unique_ptr<PluginLoader> g_pluginLoader;
static QPointer<StatusProvider> g_statusProvider;
static std::unique_ptr<ThemeManager> g_themeManager;
static QPointer<QObject> g_placeholderWindow;
static std::unique_ptr<QTimer> g_timeoutTimer;
static std::unique_ptr<QTimer> g_showPlaceholderTimer;
static std::atomic<bool> g_should_stop{false};

// 消息分发表
static MessageRouter g_message_router;

// ================================================================
// 内部辅助（匿名命名空间）
// ================================================================
namespace {

// ----- 前向声明 -----
void handleInitList(const std::string& payload);
void handleInitSessionList(const std::string& payload);
void handleSessionStateUpdate(const std::string& payload);

bool isDevMode() {
    const char* dev_mode = std::getenv("DM_DEV_MODE");
    return dev_mode && std::string(dev_mode) == "1";
}

// ================================================================
// 读取 JSON 文件 → QVariantMap
// 失败返回空 map
// ================================================================
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
        LOG_WARN("Failed to parse JSON: " + path.toStdString() +
                 " (" + error.errorString().toStdString() + ")");
        return {};
    }
    return doc.object().toVariantMap();
}

// ================================================================
// 深度合并两个 QVariantMap
//
// 规则：
//   - override 中的键覆盖 base 中的同名键
//   - 若双方的值都是 map，递归合并
//   - 否则 override 直接覆盖
// ================================================================
QVariantMap deepMergeMap(const QVariantMap& base, const QVariantMap& override) {
    QVariantMap result = base;

    for (auto it = override.begin(); it != override.end(); ++it) {
        const QString& key = it.key();
        const QVariant& val = it.value();

        if (val.canConvert<QVariantMap>() &&
            result.value(key).canConvert<QVariantMap>()) {
            QVariantMap merged = deepMergeMap(result.value(key).toMap(),
                                              val.toMap());
            result[key] = merged;
        } else {
            result[key] = val;
        }
    }
    return result;
}

// ================================================================
// 从系统模板中提取指定模式的颜色（带回退）
// ================================================================
QVariantMap extractColorsForMode(const QVariantMap& sys_template,
                                 const QString& mode) {
    QVariantMap theme = sys_template.value("theme").toMap();
    if (!theme.isEmpty()) {
        QVariantMap modes = theme.value("modes").toMap();

        QVariantMap mode_cfg = modes.value(mode).toMap();
        QVariantMap colors = mode_cfg.value("colors").toMap();
        if (!colors.isEmpty()) {
            return colors;
        }

        QString default_mode = theme.value("default_mode").toString();
        if (!default_mode.isEmpty() && default_mode != mode) {
            QVariantMap default_cfg = modes.value(default_mode).toMap();
            colors = default_cfg.value("colors").toMap();
            if (!colors.isEmpty()) {
                LOG_WARN("Theme mode '" + mode.toStdString() +
                         "' not found, falling back to default_mode '" +
                         default_mode.toStdString() + "'");
                return colors;
            }
        }

        for (auto it = modes.begin(); it != modes.end(); ++it) {
            colors = it.value().toMap().value("colors").toMap();
            if (!colors.isEmpty()) {
                LOG_WARN("Theme mode '" + mode.toStdString() +
                         "' and default_mode not found, using mode '" +
                         it.key().toStdString() + "'");
                return colors;
            }
        }
    }

    QVariantMap legacy_colors = sys_template.value("colors").toMap();
    if (!legacy_colors.isEmpty()) {
        return legacy_colors;
    }

    return {};
}

// ================================================================
// 计算启动时的窗口尺寸（主窗口 + 占位窗口）
//
// 主窗口初始尺寸：
//   w = clamp(screen_w × ratio_w, min_w, max_w)
//   h = clamp(screen_h × ratio_h, min_h, max_h)
//   用户可后续拉伸；min_window_* 是最小尺寸约束（QML 侧 minWidth/minHeight）
//
// 占位窗口尺寸：
//   scale = clamp(screen_w / ref_w, scale_min, scale_max)
//   w = placeholder_width_base × scale
//   h = placeholder_height_base × scale
//
// 说明：
//   QScreen::availableGeometry() 返回逻辑像素（已考虑系统 DPI 缩放）。
//   因此无需额外处理 DPI。
//
// 注入字段：
//   layout.initial_window_width   → 主窗口初始宽
//   layout.initial_window_height  → 主窗口初始高
//   layout.placeholder_width      → 占位窗口宽
//   layout.placeholder_height     → 占位窗口高
// ================================================================
void computeLayoutDimensions(QVariantMap& global_params) {
    QVariantMap layout = global_params.value("layout").toMap();
    if (layout.isEmpty()) {
        LOG_WARN("computeLayoutDimensions: layout is empty, skipping");
        return;
    }

    // ----- 读取基准值（含兜底默认） -----
    const int min_w         = layout.value("min_window_width", 800).toInt();
    const int min_h         = layout.value("min_window_height", 600).toInt();
    const double ratio_w    = layout.value("initial_window_width_ratio", 0.6).toDouble();
    const double ratio_h    = layout.value("initial_window_height_ratio", 0.7).toDouble();
    const int max_w         = layout.value("initial_window_max_width", 1600).toInt();
    const int max_h         = layout.value("initial_window_max_height", 1000).toInt();

    const int ph_base_w     = layout.value("placeholder_width_base", 360).toInt();
    const int ph_base_h     = layout.value("placeholder_height_base", 150).toInt();
    const int ph_ref_w      = layout.value("placeholder_scale_reference_width", 1920).toInt();
    const double ph_min     = layout.value("placeholder_scale_min", 1.0).toDouble();
    const double ph_max     = layout.value("placeholder_scale_max", 1.5).toDouble();

    // ----- 获取屏幕信息 -----
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) {
        LOG_WARN("computeLayoutDimensions: no primary screen, using min sizes");
        layout["initial_window_width"]  = min_w;
        layout["initial_window_height"] = min_h;
        layout["placeholder_width"]     = ph_base_w;
        layout["placeholder_height"]    = ph_base_h;
        global_params["layout"] = layout;
        return;
    }

    const QRect geo = screen->availableGeometry();
    const int screen_w = geo.width();
    const int screen_h = geo.height();

    // ----- 计算主窗口初始尺寸 -----
    int initial_w = static_cast<int>(screen_w * ratio_w);
    int initial_h = static_cast<int>(screen_h * ratio_h);

    if (initial_w < min_w) initial_w = min_w;
    if (initial_w > max_w) initial_w = max_w;
    if (initial_h < min_h) initial_h = min_h;
    if (initial_h > max_h) initial_h = max_h;

    layout["initial_window_width"]  = initial_w;
    layout["initial_window_height"] = initial_h;

    // ----- 计算占位窗口尺寸 -----
    double scale = (ph_ref_w > 0)
                   ? static_cast<double>(screen_w) / ph_ref_w
                   : 1.0;
    if (scale < ph_min) scale = ph_min;
    if (scale > ph_max) scale = ph_max;

    int ph_w = static_cast<int>(ph_base_w * scale);
    int ph_h = static_cast<int>(ph_base_h * scale);

    layout["placeholder_width"]  = ph_w;
    layout["placeholder_height"] = ph_h;

    global_params["layout"] = layout;

    LOG_INFO("Layout dimensions computed: screen=" +
             std::to_string(screen_w) + "x" + std::to_string(screen_h) +
             ", initial_window=" + std::to_string(initial_w) + "x" + std::to_string(initial_h) +
             ", placeholder=" + std::to_string(ph_w) + "x" + std::to_string(ph_h));
}

// ================================================================
// 硬编码兜底（系统模板 + dev 模式都读不到时使用）
//
// 含 layout 基准值——computeLayoutDimensions 依赖这些字段计算。
// ================================================================
QVariantMap buildHardcodedDefaults() {
    QVariantMap result;

    QVariantMap colors;
    colors["background"] = "#F0F0F0";
    colors["background_alt"] = "#E8E8E8";
    colors["surface"] = "#FFFFFF";
    colors["surface_alt"] = "#F5F5F5";
    colors["text_primary"] = "#333333";
    colors["text_secondary"] = "#666666";
    colors["text_muted"] = "#999999";
    colors["border"] = "#D0D0D0";
    colors["primary"] = "#3A7BD5";
    colors["primary_hover"] = "#4A8BE5";
    colors["secondary"] = "#888888";
    colors["success"] = "#4CAF50";
    colors["warning"] = "#FFC107";
    colors["error"] = "#F44336";
    colors["info"] = "#2196F3";
    result["colors"] = colors;

    QVariantMap fonts;
    fonts["family"] = "Segoe UI";
    fonts["size_small"] = 10;
    fonts["size_normal"] = 12;
    fonts["size_large"] = 14;
    fonts["size_title"] = 18;
    fonts["size_huge"] = 24;
    fonts["weight_normal"] = 400;
    fonts["weight_bold"] = 600;
    result["fonts"] = fonts;

    QVariantMap spacing;
    spacing["button_gap"] = 6;
    spacing["list_item_gap"] = 4;
    spacing["padding_small"] = 4;
    spacing["padding_normal"] = 8;
    spacing["padding_large"] = 16;
    spacing["margin_small"] = 4;
    spacing["margin_normal"] = 8;
    spacing["margin_large"] = 16;
    spacing["border_radius"] = 4;
    spacing["icon_size"] = 16;
    result["spacing"] = spacing;

    QVariantMap layout;
    layout["session_list_width"] = 200;
    layout["chat_area_padding"] = 12;
    layout["input_area_height"] = 60;
    layout["status_bar_height"] = 24;
    layout["min_window_width"] = 800;
    layout["min_window_height"] = 600;
    layout["initial_window_width_ratio"] = 0.6;
    layout["initial_window_height_ratio"] = 0.7;
    layout["initial_window_max_width"] = 1600;
    layout["initial_window_max_height"] = 1000;
    layout["placeholder_width_base"] = 360;
    layout["placeholder_height_base"] = 150;
    layout["placeholder_scale_reference_width"] = 1920;
    layout["placeholder_scale_min"] = 1.0;
    layout["placeholder_scale_max"] = 1.5;
    result["layout"] = layout;

    QVariantMap animation;
    animation["duration_short"] = 150;
    animation["duration_normal"] = 300;
    animation["duration_long"] = 500;
    animation["easing_type"] = "easeInOut";
    result["animation"] = animation;

    return result;
}

// ================================================================
// 加载全局参数（供 QML 使用的扁平结构）
//
// 加载顺序：
//   1. 系统模板（生产路径；dev 路径回退）
//   2. 用户偏好（data/theme_preferences.json，可能不存在）
//   3. 主题模式：用户主题模式 > 系统 default_mode > "light"
//   4. 从系统模板提取对应模式的基础 colors（带回退）
//   5. 深度合并（用户偏好优先）每个字段
//   6. 计算窗口尺寸（computeLayoutDimensions）
//   7. 返回 { colors, fonts, spacing, layout, animation }
// ================================================================
QVariantMap loadGlobalParams() {
    // ----- 1. 读系统模板 -----
    QVariantMap sys_template;

    {
        QString sys_path = QString::fromStdString(
            common::pathFromRoot("plugins/system/dream_machine_default/config/global_params.json"));
        sys_template = readJsonFile(sys_path);
        if (!sys_template.isEmpty()) {
            LOG_INFO("global_params.json loaded from: " + sys_path.toStdString());
        }
    }

    if (sys_template.isEmpty() && isDevMode()) {
        QString project_root = QDir::currentPath();
        QDir proj_dir(project_root);
        if (proj_dir.dirName() == "bin") {
            proj_dir.cdUp();
            proj_dir.cdUp();
        }
        QString dev_path = proj_dir.filePath("src/default_plugin/config/global_params.json");
        sys_template = readJsonFile(dev_path);
        if (!sys_template.isEmpty()) {
            LOG_INFO("global_params.json loaded from (dev): " + dev_path.toStdString());
        }
    }

    // ----- 完全读不到 → 硬编码兜底 + 计算尺寸 -----
    if (sys_template.isEmpty()) {
        LOG_WARN("System template not found, using hardcoded defaults");
        QVariantMap result = buildHardcodedDefaults();
        computeLayoutDimensions(result);
        return result;
    }

    // ----- 2. 读用户偏好 -----
    QString user_path = QString::fromStdString(
        common::pathFromRoot("data/theme_preferences.json"));
    QVariantMap user_prefs = readJsonFile(user_path);
    if (!user_prefs.isEmpty()) {
        LOG_INFO("theme_preferences.json loaded from: " + user_path.toStdString());
    }

    // ----- 3. 确定主题模式 -----
    QString mode = user_prefs.value("theme_mode").toString();
    if (mode.isEmpty()) {
        QVariantMap theme = sys_template.value("theme").toMap();
        mode = theme.value("default_mode").toString();
    }
    if (mode.isEmpty()) {
        mode = "light";
    }
    LOG_INFO("Theme mode resolved to: " + mode.toStdString());

    // ----- 4. 提取基础 colors（带回退） -----
    QVariantMap base_colors = extractColorsForMode(sys_template, mode);

    // ----- 5. 深度合并每类字段 -----
    QVariantMap result;
    result["colors"]    = deepMergeMap(base_colors,
                                       user_prefs.value("colors").toMap());
    result["fonts"]     = deepMergeMap(sys_template.value("fonts").toMap(),
                                       user_prefs.value("fonts").toMap());
    result["spacing"]   = deepMergeMap(sys_template.value("spacing").toMap(),
                                       user_prefs.value("spacing").toMap());
    result["layout"]    = deepMergeMap(sys_template.value("layout").toMap(),
                                       user_prefs.value("layout").toMap());
    result["animation"] = deepMergeMap(sys_template.value("animation").toMap(),
                                       user_prefs.value("animation").toMap());

    // ----- 6. 计算窗口尺寸（注入 initial_window_* / placeholder_*） -----
    computeLayoutDimensions(result);

    return result;
}

void showPlaceholderWindow() {
    if (g_placeholderWindow) {
        QQuickWindow* win = qobject_cast<QQuickWindow*>(g_placeholderWindow.data());
        if (win && !win->isVisible()) {
            win->setVisible(true);
            LOG_INFO("Placeholder window shown due to loading delay");
        }
    }
    if (g_statusProvider) {
        g_statusProvider->setStatusText("正在加载插件...");
        g_statusProvider->setLoading(true);
    }
}

void hidePlaceholderWindow() {
    if (g_showPlaceholderTimer) {
        g_showPlaceholderTimer->stop();
    }
    if (g_placeholderWindow) {
        g_placeholderWindow->deleteLater();
        g_placeholderWindow.clear();
        LOG_INFO("Placeholder window destroyed");
    }
}

void handleInitList(const std::string& payload) {
    if (!g_pluginLoader || !g_statusProvider) {
        LOG_ERROR("PluginLoader or StatusProvider not initialized");
        return;
    }

    g_statusProvider->setStatusText("正在加载插件框架...");
    LOG_INFO("Processing INIT_LIST payload...");
    bool success = g_pluginLoader->loadFromInitList(payload);

    if (success) {
        g_statusProvider->setStatusText("插件加载成功");
        g_statusProvider->setLoading(false);
        hidePlaceholderWindow();

        if (g_timeoutTimer) {
            g_timeoutTimer->stop();
            LOG_INFO("Global timeout timer stopped");
        }

        if (g_pluginLoader->isFrameworkLoaded()) {
            LOG_INFO("Framework loaded successfully");
        }

        InitListAckMessage ack;
        ack.status = "ok";
        std::string ack_json = serializeInitListAck(ack);
        if (g_pipe && g_pipe->isValid() && g_pipe->isConnected()) {
            g_pipe->writeLine(ack_json);
            LOG_INFO("Sent INIT_LIST_ACK (success)");
        }
    } else {
        g_statusProvider->setStatusText("插件加载失败");
        g_statusProvider->setErrorText("请检查日志或插件文件完整性");
        g_statusProvider->setLoading(false);
        g_statusProvider->setShowExitButton(true);

        if (g_showPlaceholderTimer) {
            g_showPlaceholderTimer->stop();
        }
        showPlaceholderWindow();

        LOG_ERROR("Failed to load plugins from INIT_LIST");
        InitListAckMessage ack;
        ack.status = "error";
        ack.error = "Failed to load plugins";
        std::string ack_json = serializeInitListAck(ack);
        if (g_pipe && g_pipe->isValid() && g_pipe->isConnected()) {
            g_pipe->writeLine(ack_json);
            LOG_INFO("Sent INIT_LIST_ACK (error)");
        }
    }
}

void handleInitSessionList(const std::string& payload) {
    if (!g_sessionManager) {
        return;
    }
    auto msg = parseInitSessionList(payload);
    if (!msg.has_value()) {
        LOG_WARN("Failed to parse INIT_SESSION_LIST");
        return;
    }
    g_sessionManager->clearAll();
    LOG_INFO("Initialized session list (empty)");
}

void handleSessionStateUpdate(const std::string& payload) {
    if (!g_sessionManager) {
        return;
    }

    auto msg = parseSessionStateUpdate(payload);
    if (!msg.has_value()) {
        LOG_WARN("Failed to parse SESSION_STATE_UPDATE message");
        return;
    }

    LOG_INFO("Session state update: " + msg->session_id + " -> " + msg->state);
    g_sessionManager->updateSessionState(msg->session_id, msg->state);
}

// ================================================================
// 消息 handler 注册
// ================================================================
void registerMessageHandlers(MessageRouter& router) {
    router.register_handler(msg_types::SHUTDOWN,
        [](const std::string& payload, void* /*ctx*/) {
            auto shutdown_msg = parseShutdown(payload);
            std::string reason = shutdown_msg.has_value()
                                 ? shutdown_msg->reason
                                 : std::string(shutdown_reason::PEER_EXIT);

            LOG_INFO("Received SHUTDOWN from launcher, reason=" + reason +
                     ", quitting GUI");

            g_should_stop = true;
            QApplication::quit();
        });

    router.register_handler(msg_types::INIT_LIST,
        [](const std::string& payload, void* /*ctx*/) {
            handleInitList(payload);
        });

    router.register_handler(msg_types::SESSION_STATE_UPDATE,
        [](const std::string& payload, void* /*ctx*/) {
            handleSessionStateUpdate(payload);
        });

    router.register_handler(msg_types::INIT_SESSION_LIST,
        [](const std::string& payload, void* /*ctx*/) {
            handleInitSessionList(payload);
        });
}

// ================================================================
// 管道轮询（QTimer 50ms 触发）
// ================================================================
void pollPipe() {
    if (!g_pipe || g_should_stop) {
        QApplication::quit();
        return;
    }

    if (!g_pipe->isValid()) {
        QApplication::quit();
        return;
    }

    if (g_pipe->isBroken()) {
        LOG_WARN("Pipe broken, quitting GUI");
        QApplication::quit();
        return;
    }

    DWORD bytes_available = 0;
    PipeResult peek_result = g_pipe->peekAvailable(bytes_available);

    if (peek_result == PipeResult::PIPE_BROKEN) {
        LOG_WARN("Pipe broken (peek), quitting GUI");
        QApplication::quit();
        return;
    }

    if (peek_result == PipeResult::PIPE_OK && bytes_available > 0) {
        std::string message;
        PipeResult read_result = g_pipe->readLine(message, 3000);

        if (read_result == PipeResult::PIPE_OK) {
            LOG_INFO("Received: " + message);

            std::string type, cmd, payload;
            if (parseBaseMessage(message, type, cmd, payload)) {
                if (!g_message_router.dispatch(type, payload, nullptr)) {
                    LOG_WARN("Unhandled message type: " + type);
                }
            } else {
                LOG_WARN("Failed to parse base message");
            }

        } else if (read_result == PipeResult::PIPE_BROKEN) {
            LOG_WARN("Pipe broken (read), quitting GUI");
            QApplication::quit();
        } else if (read_result == PipeResult::PIPE_TIMEOUT) {
            LOG_WARN("Read timeout, will retry");
        }
    }
}

void onTimeout() {
    if (g_showPlaceholderTimer) {
        g_showPlaceholderTimer->stop();
    }
    showPlaceholderWindow();

    if (g_statusProvider) {
        g_statusProvider->setStatusText("加载超时");
        g_statusProvider->setErrorText("未收到 launcher 的插件列表，请检查 launcher 是否正常运行");
        g_statusProvider->setLoading(false);
        g_statusProvider->setShowExitButton(true);
    }
    LOG_ERROR("Timeout waiting for INIT_LIST");
}

} // namespace

// ================================================================
// main 入口
//
// 路径策略（Step 0 路径修正）：
//   所有运行时资源基于可执行文件所在目录，不依赖 CWD。
//
// 日志生命周期：
//   - 启动：setProcessName → setLogDirectory → archiveLastSessionIfDirty → 开始日志
//   - 退出：最后一条日志 → markCleanExit → return 0
//
// QML 样式（Step 1 警告修复）：
//   QQuickStyle::setStyle("Fusion") 使 Qt Quick Controls 2 允许
//   覆盖 background / contentItem 等属性。
//
// 主题系统（Step 3 / Step 4）：
//   - loadGlobalParams() 启动时读系统模板 + 用户偏好，注入 QML
//   - ThemeManager 提供运行时切换（写入 data/theme_preferences.json）
//   - 切换需重启生效（不重载 QML）
//
// 窗口尺寸（Step 4.5）：
//   - 启动时基于主屏幕分辨率计算：
//       主窗口初始尺寸（基于比例 + 上下限）
//       占位窗口尺寸（基于基准值 × 分辨率缩放）
//   - QML 侧读 globalParams.layout.initial_window_* / placeholder_*
//   - 用户可自由拉伸；最小尺寸由 min_window_* 约束
//
// 失败路径不写 .clean_exit：它们不是正常会话，下次启动应被归档。
// ================================================================
int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("gui");

    Logger::instance().setLogDirectory(common::pathFromRoot("logs"));

    const bool archived_prev = Logger::instance().archiveLastSessionIfDirty();

    LOG_INFO("=== Dream Machine GUI starting ===");

    if (archived_prev) {
        LOG_INFO("Previous session logs archived to logs/crashes/");
    }

    registerMessageHandlers(g_message_router);

    std::string parent_pid_str = common::getArgValue(argc, argv, "--parent-pid");
    DWORD expected_parent_pid = 0;
    if (!parent_pid_str.empty()) {
        expected_parent_pid = static_cast<DWORD>(std::stoul(parent_pid_str));
    }

    if (!common::verifyParentPid(expected_parent_pid)) {
        return 1;
    }

    std::string pipe_name_str = pipe_names::launcher_gui();

    std::wstring pipe_name = common::utf8ToWide(pipe_name_str);

    LOG_INFO("Connecting to launcher pipe: " + pipe_name_str);

    auto pipe = std::make_unique<NamedPipe>();
    if (!pipe->connect(pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to launcher pipe, exiting");
        return 1;
    }

    LOG_INFO("Connected to launcher pipe");
    g_pipe = std::move(pipe);

    RegisterMessage reg_msg;
    reg_msg.process = "gui";
    std::string register_msg = serializeRegister(reg_msg);

    if (g_pipe->writeLine(register_msg) != PipeResult::PIPE_OK) {
        LOG_ERROR("Failed to send registration message");
    } else {
        LOG_INFO("Registration message sent: " + register_msg);
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName("Dream Machine");
    QApplication::setOrganizationName("DreamMachine");
    app.setStyle("Fusion");
    QQuickStyle::setStyle("Fusion");
    LOG_INFO("QApplication initialized with Fusion style (Widgets + Quick Controls)");

    auto statusProvider = std::make_unique<StatusProvider>();
    g_statusProvider = statusProvider.get();

    auto sessionManager = std::make_unique<SessionStateManager>();
    g_sessionManager = sessionManager.get();

    auto themeManager = std::make_unique<ThemeManager>();
    g_themeManager = std::move(themeManager);

    auto engine = std::make_unique<QQmlApplicationEngine>();
    g_engine = engine.get();

    QVariantMap globalParams = loadGlobalParams();
    engine->rootContext()->setContextProperty("globalParams", globalParams);
    engine->rootContext()->setContextProperty("statusProvider", statusProvider.get());
    engine->rootContext()->setContextProperty("sessionManager", sessionManager.get());
    engine->rootContext()->setContextProperty("themeManager", g_themeManager.get());

    QObject::connect(engine.get(), &QQmlApplicationEngine::warnings,
        [](const QList<QQmlError>& warnings) {
            for (const auto& error : warnings) {
                LOG_ERROR("QML warning: " + error.toString().toStdString());
            }
        });

    QObject::connect(engine.get(), &QQmlApplicationEngine::objectCreated,
        [](QObject* obj, const QUrl& objUrl) {
            if (obj) {
                LOG_INFO("QML object created for: " + objUrl.toString().toStdString());
            } else {
                LOG_ERROR("QML object creation failed for: " + objUrl.toString().toStdString());
            }
        });

    auto pluginLoader = std::make_unique<PluginLoader>();
    g_pluginLoader = std::move(pluginLoader);
    g_pluginLoader->setEngine(engine.get());

    bool dev_mode = isDevMode();
    if (dev_mode) {
        LOG_INFO("=== DEVELOPMENT MODE ENABLED ===");
        statusProvider->setStatusText("开发模式 - 从源码加载");
    }

    QString app_dir = QCoreApplication::applicationDirPath();
    LOG_INFO("Application directory: " + app_dir.toStdString());

    QString placeholder_path;
    if (dev_mode) {
        QString project_root = QDir::currentPath();
        QDir proj_dir(project_root);
        if (proj_dir.dirName() == "bin") {
            proj_dir.cdUp();
            proj_dir.cdUp();
        }
        placeholder_path = proj_dir.filePath("src/gui/qml/main.qml");
        QUrl url = QUrl::fromLocalFile(placeholder_path);
        if (QFile::exists(placeholder_path)) {
            engine->load(url);
            LOG_INFO("Development placeholder loaded from: " + placeholder_path.toStdString());
        } else {
            LOG_WARN("Development placeholder not found, trying build directory");
            placeholder_path = QDir(app_dir).filePath("src/gui/qml/main.qml");
            engine->load(QUrl::fromLocalFile(placeholder_path));
        }
    } else {
        QDir base_dir2(app_dir);
        bool up1 = base_dir2.cdUp();
        bool up2 = base_dir2.cdUp();
        if (up1 && up2) {
            placeholder_path = base_dir2.filePath("src/gui/qml/main.qml");
        } else {
            placeholder_path = app_dir + "/../src/gui/qml/main.qml";
        }
        QUrl url = QUrl::fromLocalFile(placeholder_path);
        if (QFile::exists(placeholder_path)) {
            engine->load(url);
            LOG_INFO("Placeholder loaded from: " + placeholder_path.toStdString());
        } else {
            LOG_ERROR("Placeholder QML not found at: " + placeholder_path.toStdString());
            engine->load(QUrl::fromLocalFile("../src/gui/qml/main.qml"));
        }
    }

    if (!engine->rootObjects().isEmpty()) {
        g_placeholderWindow = engine->rootObjects().first();
        g_placeholderWindow->setObjectName("placeholder");
        LOG_INFO("Placeholder window created (initially hidden)");
    }

    auto showTimer = std::make_unique<QTimer>();
    g_showPlaceholderTimer = std::move(showTimer);
    g_showPlaceholderTimer->setSingleShot(true);
    g_showPlaceholderTimer->setInterval(500);
    QObject::connect(g_showPlaceholderTimer.get(), &QTimer::timeout, showPlaceholderWindow);
    g_showPlaceholderTimer->start();
    LOG_INFO("Placeholder show timer started (500ms)");

    auto timeoutTimer = std::make_unique<QTimer>();
    g_timeoutTimer = std::move(timeoutTimer);
    g_timeoutTimer->setSingleShot(true);
    g_timeoutTimer->setInterval(10000);
    QObject::connect(g_timeoutTimer.get(), &QTimer::timeout, onTimeout);
    g_timeoutTimer->start();

    QTimer pollTimer;
    pollTimer.setInterval(50);
    QObject::connect(&pollTimer, &QTimer::timeout, pollPipe);
    pollTimer.start();

    LOG_INFO("Entering Qt event loop...");
    int result = QApplication::exec();

    LOG_INFO("Shutting down GUI...");
    pollTimer.stop();

    if (g_showPlaceholderTimer) {
        g_showPlaceholderTimer->stop();
    }
    if (g_timeoutTimer) {
        g_timeoutTimer->stop();
    }

    if (g_pluginLoader) {
        g_pluginLoader->reset();
    }

    if (g_placeholderWindow) {
        g_placeholderWindow->deleteLater();
        g_placeholderWindow.clear();
    }

    g_pipe.reset();
    g_showPlaceholderTimer.reset();
    g_timeoutTimer.reset();

    g_sessionManager.clear();
    g_engine.clear();
    g_pluginLoader.reset();
    g_statusProvider.clear();
    g_themeManager.reset();

    LOG_INFO("=== GUI exited with code " + std::to_string(result) + " ===");

    Logger::instance().markCleanExit();

    return 0;
}