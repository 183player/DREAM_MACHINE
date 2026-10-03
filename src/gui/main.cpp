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
#include "error_codes.h"
#include "error_notify_bridge.h"

// dm_signal（A.8 装配）
#include "signal_bus.h"
#include "message_bridge.h"
#include "signal_types.h"
#include "signal_strings.h"

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
static QPointer<QObject> g_loadingWindow;
static std::unique_ptr<QTimer> g_timeoutTimer;
static std::unique_ptr<QTimer> g_showLoadingTimer;
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
void attachSignalBus();
void detachSignalBus();
void publish_error(ErrorCode code,
                   ErrorSeverity severity,
                   const std::string& message,
                   const std::string& details = "",
                   const std::string& session_id = "");

bool isDevMode() {
    const char* dev_mode = std::getenv("DM_DEV_MODE");
    return dev_mode && std::string(dev_mode) == "1";
}

// ================================================================
// publish_error：通过 SignalBus 发布结构化错误（A.11.2b-3 B3 接线）
// ================================================================
void publish_error(ErrorCode code,
                   ErrorSeverity severity,
                   const std::string& message,
                   const std::string& details,
                   const std::string& session_id) {
    signal::SignalPayload payload;
    payload.type = signal::SignalType::SGT_ERROR_NOTIFY;
    payload.level = static_cast<signal::SignalLevel>(
        error_severity_to_signal_level_int(severity));
    payload.timestamp_ms = signal::now_ms();
    payload.description = message;
    payload.detail = details;
    if (!session_id.empty()) {
        payload.session_id = session_id;
    }
    payload.extra["error_code"] = errorCodeToString(code);

    signal::SignalBus::instance().publish(payload);
}

// ================================================================
// dm_signal 装配（A.8 装配 + A.11.2b-3 Sender 完整实现）
// ================================================================
void attachSignalBus() {
    Logger::attach_to_signal_bus();
    signal::SignalBus::instance().subscribe(&signal::MessageBridge::instance());

    (void)signal::MessageBridge::instance().add_sender(
        "launcher",
        [](const signal::SignalPayload& payload) -> bool {
            if (!g_pipe || !g_pipe->isValid()) {
                return false;
            }

            ErrorNotifyMessage notify;
            notify.source = "gui";
            notify.severity = signal::signal_level_to_string(payload.level);
            notify.message = payload.description;

            std::string details = payload.detail;
            auto it_code = payload.extra.find("error_code");
            if (it_code != payload.extra.end() && !it_code->second.empty()) {
                if (!details.empty()) {
                    details += " | ";
                }
                details += std::string("code=") + it_code->second;
            }
            if (!details.empty()) {
                notify.details = details;
            }

            std::string json = serializeErrorNotify(notify);
            PipeResult result = g_pipe->writeLine(json);
            if (result == PipeResult::PIPE_OK) {
                LOG_INFO("ErrorNotify sent to launcher: " + payload.description);
                return true;
            }
            LOG_WARN("Failed to send ErrorNotify to launcher");
            return false;
        },
        [](const signal::SignalPayload& payload) -> bool {
            return payload.type == signal::SignalType::SGT_ERROR_NOTIFY;
        });

    LOG_INFO("dm_signal attached: Logger + MessageBridge (gui)");
}

void detachSignalBus() {
    signal::MessageBridge::instance().remove_all_senders();
    Logger::detach_from_signal_bus();
    LOG_INFO("dm_signal detached (gui)");
}

// ================================================================
// 读取 JSON 文件 → QVariantMap
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
// 计算启动时的窗口尺寸（主窗口 + loading 窗口）
// ================================================================
void computeLayoutDimensions(QVariantMap& global_params) {
    QVariantMap layout = global_params.value("layout").toMap();
    if (layout.isEmpty()) {
        LOG_WARN("computeLayoutDimensions: layout is empty, skipping");
        return;
    }

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

    int initial_w = static_cast<int>(screen_w * ratio_w);
    int initial_h = static_cast<int>(screen_h * ratio_h);

    if (initial_w < min_w) initial_w = min_w;
    if (initial_w > max_w) initial_w = max_w;
    if (initial_h < min_h) initial_h = min_h;
    if (initial_h > max_h) initial_h = max_h;

    layout["initial_window_width"]  = initial_w;
    layout["initial_window_height"] = initial_h;

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
// 硬编码兜底
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
// 加载全局参数
//
// 路径策略（不依赖 CWD）：
//   1. 系统插件模板：common::pathFromRoot("plugins/system/...")（部署+编译版）
//   2. dev 兜底：getProjectRoot() + 源码路径（仅 DM_DEV_MODE=1）
//   3. 兜底：硬编码 + publish_error
// ================================================================
QVariantMap loadGlobalParams() {
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
        QString root = QString::fromStdString(common::getProjectRootPath());
        if (!root.isEmpty()) {
            QString dev_path = QDir(root).filePath("src/default_plugin/config/global_params.json");
            sys_template = readJsonFile(dev_path);
            if (!sys_template.isEmpty()) {
                LOG_INFO("global_params.json loaded from (dev): " + dev_path.toStdString());
            }
        }
    }

    if (sys_template.isEmpty()) {
        LOG_WARN("System template not found, using hardcoded defaults");
        publish_error(ErrorCode::CONFIG_NOT_FOUND,
                      ErrorSeverity::WARNING,
                      "System template not found, using hardcoded defaults",
                      "plugins/system/dream_machine_default/config/global_params.json");
        QVariantMap result = buildHardcodedDefaults();
        computeLayoutDimensions(result);
        return result;
    }

    QString user_path = QString::fromStdString(
        common::pathFromRoot("data/theme_preferences.json"));
    QVariantMap user_prefs = readJsonFile(user_path);
    if (!user_prefs.isEmpty()) {
        LOG_INFO("theme_preferences.json loaded from: " + user_path.toStdString());
    }

    QString mode = user_prefs.value("theme_mode").toString();
    if (mode.isEmpty()) {
        QVariantMap theme = sys_template.value("theme").toMap();
        mode = theme.value("default_mode").toString();
    }
    if (mode.isEmpty()) {
        mode = "light";
    }
    LOG_INFO("Theme mode resolved to: " + mode.toStdString());

    QVariantMap base_colors = extractColorsForMode(sys_template, mode);

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

    computeLayoutDimensions(result);

    return result;
}

void showLoadingWindow() {
    if (g_loadingWindow) {
        QQuickWindow* win = qobject_cast<QQuickWindow*>(g_loadingWindow.data());
        if (win && !win->isVisible()) {
            win->setVisible(true);
            LOG_INFO("Loading window shown due to loading delay");
        }
    }
    if (g_statusProvider) {
        g_statusProvider->setStatusText("正在加载插件...");
        g_statusProvider->setLoading(true);
    }
}

void hideLoadingWindow() {
    if (g_showLoadingTimer) {
        g_showLoadingTimer->stop();
    }
    if (g_loadingWindow) {
        g_loadingWindow->deleteLater();
        g_loadingWindow.clear();
        LOG_INFO("Loading window destroyed");
    }
}

// ================================================================
// handleInitList
// ================================================================
void handleInitList(const std::string& payload) {
    if (!g_pluginLoader || !g_statusProvider) {
        LOG_ERROR("PluginLoader or StatusProvider not initialized");
        publish_error(ErrorCode::PLUGIN_LOAD_FAILED,
                      ErrorSeverity::FATAL,
                      "PluginLoader or StatusProvider not initialized",
                      "handleInitList invoked before initialization");
        return;
    }

    g_statusProvider->setStatusText("正在加载插件框架...");
    LOG_INFO("Processing INIT_LIST payload...");
    bool success = g_pluginLoader->loadFromInitList(payload);

    if (success) {
        g_statusProvider->setStatusText("插件加载成功");
        g_statusProvider->setLoading(false);
        hideLoadingWindow();

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

        if (g_showLoadingTimer) {
            g_showLoadingTimer->stop();
        }
        showLoadingWindow();

        LOG_ERROR("Failed to load plugins from INIT_LIST");
        publish_error(ErrorCode::PLUGIN_LOAD_FAILED,
                      ErrorSeverity::WARNING,
                      "Failed to load plugins from INIT_LIST",
                      "loadFromInitList returned false");

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
        publish_error(ErrorCode::JSON_ERROR,
                      ErrorSeverity::WARNING,
                      "Failed to parse INIT_SESSION_LIST",
                      "parseInitSessionList returned nullopt");
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
        publish_error(ErrorCode::JSON_ERROR,
                      ErrorSeverity::WARNING,
                      "Failed to parse SESSION_STATE_UPDATE",
                      "parseSessionStateUpdate returned nullopt");
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
        publish_error(ErrorCode::PIPE_BROKEN,
                      ErrorSeverity::FATAL,
                      "Launcher pipe broken",
                      "isBroken() returned true");
        QApplication::quit();
        return;
    }

    DWORD bytes_available = 0;
    PipeResult peek_result = g_pipe->peekAvailable(bytes_available);

    if (peek_result == PipeResult::PIPE_BROKEN) {
        LOG_WARN("Pipe broken (peek), quitting GUI");
        publish_error(ErrorCode::PIPE_BROKEN,
                      ErrorSeverity::FATAL,
                      "Launcher pipe broken (peek)",
                      "peekAvailable returned PIPE_BROKEN");
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
                publish_error(ErrorCode::JSON_ERROR,
                              ErrorSeverity::WARNING,
                              "Failed to parse base message from launcher",
                              "parseBaseMessage returned false");
            }

        } else if (read_result == PipeResult::PIPE_BROKEN) {
            LOG_WARN("Pipe broken (read), quitting GUI");
            publish_error(ErrorCode::PIPE_BROKEN,
                          ErrorSeverity::FATAL,
                          "Launcher pipe broken (read)",
                          "readLine returned PIPE_BROKEN");
            QApplication::quit();
        } else if (read_result == PipeResult::PIPE_TIMEOUT) {
            LOG_WARN("Read timeout, will retry");
        }
    }
}

void onTimeout() {
    if (g_showLoadingTimer) {
        g_showLoadingTimer->stop();
    }
    showLoadingWindow();

    if (g_statusProvider) {
        g_statusProvider->setStatusText("加载超时");
        g_statusProvider->setErrorText("未收到 launcher 的插件列表，请检查 launcher 是否正常运行");
        g_statusProvider->setLoading(false);
        g_statusProvider->setShowExitButton(true);
    }
    LOG_ERROR("Timeout waiting for INIT_LIST");
    publish_error(ErrorCode::TIMEOUT,
                  ErrorSeverity::WARNING,
                  "Timeout waiting for INIT_LIST",
                  "10s timer expired before INIT_LIST received");
}

} // namespace

// ================================================================
// main 入口
//
// 路径策略（部署即用、跨机可移植）：
//   - 所有运行时资源基于 exe 目录（common::pathFromRoot）
//   - 无绝对路径硬编码；复制 bin/ 整体到任意位置可运行
//   - loading 窗口是**必要组件**（随系统插件部署）
//
// 本分片合并改动：
//   A.8        dm_signal 装配
//   A.11.2b-3  B3 错误结构化接线
//   A.5        C5 GUI 路径统一（loading 窗口纳入默认插件）
// ================================================================
int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("gui");

    Logger::instance().setLogDirectory(common::pathFromRoot("logs"));

    const bool archived_prev = Logger::instance().archiveLastSessionIfDirty();

    LOG_INFO("=== Dream Machine GUI starting ===");

    if (archived_prev) {
        LOG_INFO("Previous session logs archived to logs/crashes/");
    }

    attachSignalBus();

    registerMessageHandlers(g_message_router);

    std::string parent_pid_str = common::getArgValue(argc, argv, "--parent-pid");
    DWORD expected_parent_pid = 0;
    if (!parent_pid_str.empty()) {
        expected_parent_pid = static_cast<DWORD>(std::stoul(parent_pid_str));
    }

    if (!common::verifyParentPid(expected_parent_pid)) {
        publish_error(ErrorCode::PROCESS_LAUNCH_FAILED,
                      ErrorSeverity::FATAL,
                      "Parent PID verification failed",
                      "verifyParentPid returned false");
        detachSignalBus();
        return 1;
    }

    std::string pipe_name_str = pipe_names::launcher_gui();

    std::wstring pipe_name = common::utf8ToWide(pipe_name_str);

    LOG_INFO("Connecting to launcher pipe: " + pipe_name_str);

    auto pipe = std::make_unique<NamedPipe>();
    if (!pipe->connect(pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to launcher pipe, exiting");
        publish_error(ErrorCode::PIPE_CONNECT_FAILED,
                      ErrorSeverity::FATAL,
                      "Failed to connect to launcher pipe",
                      pipe_name_str);
        detachSignalBus();
        return 1;
    }

    LOG_INFO("Connected to launcher pipe");
    g_pipe = std::move(pipe);

    RegisterMessage reg_msg;
    reg_msg.process = "gui";
    std::string register_msg = serializeRegister(reg_msg);

    if (g_pipe->writeLine(register_msg) != PipeResult::PIPE_OK) {
        LOG_ERROR("Failed to send registration message");
        publish_error(ErrorCode::PIPE_ERROR,
                      ErrorSeverity::FATAL,
                      "Failed to send registration message to launcher",
                      "writeLine returned non-PIPE_OK");
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

    LOG_INFO("Application directory: " +
             QCoreApplication::applicationDirPath().toStdString());

    // A.5.5：loading 窗口路径解析
    //   主路径：<exe_dir>/plugins/system/dream_machine_default/qml/loading.qml
    //   （CMake 复制 default_plugin 时自动带入；部署版随 bin/ 一起走）
    //   兜底：源码树 src/default_plugin/qml/loading.qml（仅 DM_DEV_MODE=1）
    const char* LOADING_REL =
        "plugins/system/dream_machine_default/qml/loading.qml";
    QString loading_path = QString::fromStdString(common::pathFromRoot(LOADING_REL));

    if (!QFile::exists(loading_path) && isDevMode()) {
        QString root = QString::fromStdString(common::getProjectRootPath());
        if (!root.isEmpty()) {
            QString dev_path = QDir(root).filePath("src/default_plugin/qml/loading.qml");
            if (QFile::exists(dev_path)) {
                LOG_INFO("Loading QML found (dev): " + dev_path.toStdString());
                loading_path = dev_path;
            }
        }
    }

    if (!QFile::exists(loading_path)) {
        LOG_ERROR("Loading QML not found: " + loading_path.toStdString());
        publish_error(ErrorCode::CONFIG_NOT_FOUND,
                      ErrorSeverity::FATAL,
                      "Loading QML not found",
                      loading_path.toStdString());
        detachSignalBus();
        return 1;
    }

    LOG_INFO("Loading window loaded from: " + loading_path.toStdString());
    engine->load(QUrl::fromLocalFile(loading_path));

    if (!engine->rootObjects().isEmpty()) {
        g_loadingWindow = engine->rootObjects().first();
        g_loadingWindow->setObjectName("loading");
        LOG_INFO("Loading window created (initially hidden)");
    }

    auto showTimer = std::make_unique<QTimer>();
    g_showLoadingTimer = std::move(showTimer);
    g_showLoadingTimer->setSingleShot(true);
    g_showLoadingTimer->setInterval(500);
    QObject::connect(g_showLoadingTimer.get(), &QTimer::timeout, showLoadingWindow);
    g_showLoadingTimer->start();
    LOG_INFO("Loading show timer started (500ms)");

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

    if (g_showLoadingTimer) {
        g_showLoadingTimer->stop();
    }
    if (g_timeoutTimer) {
        g_timeoutTimer->stop();
    }

    if (g_pluginLoader) {
        g_pluginLoader->reset();
    }

    if (g_loadingWindow) {
        g_loadingWindow->deleteLater();
        g_loadingWindow.clear();
    }

    g_pipe.reset();
    g_showLoadingTimer.reset();
    g_timeoutTimer.reset();

    g_sessionManager.clear();
    g_engine.clear();
    g_pluginLoader.reset();
    g_statusProvider.clear();
    g_themeManager.reset();

    detachSignalBus();

    LOG_INFO("=== GUI exited with code " + std::to_string(result) + " ===");

    Logger::instance().markCleanExit();

    return 0;
}