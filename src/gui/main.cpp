// src/gui/main.cpp
#include "logger.h"
#include "pipe.h"
#include "constants.h"
#include "messages.h"
#include "message_router.h"
#include "session_state_manager.h"
#include "plugin_loader.h"
#include "status_provider.h"
#include "common_utils.h"

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
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
// 注：阶段 1.7 C4 起不再 using dream_machine::common——
//     新增的 utf8ToWide / wideToUtf8 用 common:: 前缀显式调用。

// ================================================================
// 未来重审点（依据 DREAM_MACHINE_CONCURRENCY_MODEL_BOUNDARY 专家裁决 Q4）：
//
//   1. 若 GUI 引入 QJSEngine 工作线程（插件脚本执行），需重审：
//        - Qt 对象访问必须通过 QMetaObject::invokeMethod 跨线程；
//        - Logger 的 thread_local channel 保证各线程日志隔离（Q5 已裁决）；
//        - g_pipe 改 std::shared_ptr<NamedPipe> 保证生命周期；
//   2. Logger 的 mutex_ 保留（Q5 裁决）——即使引入线程也无需重构 Logger 本身。
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
static QPointer<QObject> g_placeholderWindow;
static std::unique_ptr<QTimer> g_timeoutTimer;
static std::unique_ptr<QTimer> g_showPlaceholderTimer;
static std::atomic<bool> g_should_stop{false};

// 消息分发表（阶段 1.7 C1）
//
// 迁移了全部 4 个 handler（SHUTDOWN / INIT_LIST / SESSION_STATE_UPDATE /
// INIT_SESSION_LIST）。gui 无 fallback 特例。
//
// 回退方式：删除本变量 + registerMessageHandlers 调用，
//          并将 pollPipe 恢复为原始 if-else 即可。
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

QVariantMap loadGlobalParams() {
    QVariantMap result;

    QString config_path = "plugins/system/dream_machine_default/config/global_params.json";
    if (QFile::exists(config_path)) {
        QFile file(config_path);
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray data = file.readAll();
            file.close();
            QJsonParseError error;
            QJsonDocument doc = QJsonDocument::fromJson(data, &error);
            if (error.error == QJsonParseError::NoError && doc.isObject()) {
                result = doc.object().toVariantMap();
                LOG_INFO("global_params.json loaded from: " + config_path.toStdString());
                return result;
            }
        }
    }

    if (isDevMode()) {
        QString project_root = QDir::currentPath();
        QDir proj_dir(project_root);
        if (proj_dir.dirName() == "bin") {
            proj_dir.cdUp();
            proj_dir.cdUp();
        }
        config_path = proj_dir.filePath("src/default_plugin/config/global_params.json");
        if (QFile::exists(config_path)) {
            QFile file(config_path);
            if (file.open(QIODevice::ReadOnly)) {
                QByteArray data = file.readAll();
                file.close();
                QJsonParseError error;
                QJsonDocument doc = QJsonDocument::fromJson(data, &error);
                if (error.error == QJsonParseError::NoError && doc.isObject()) {
                    result = doc.object().toVariantMap();
                    LOG_INFO("global_params.json loaded from (dev): " + config_path.toStdString());
                    return result;
                }
            }
        }
    }

    LOG_WARN("global_params.json not found, using minimal defaults");
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
    result["layout"] = layout;

    QVariantMap animation;
    animation["duration_short"] = 150;
    animation["duration_normal"] = 300;
    animation["duration_long"] = 500;
    animation["easing_type"] = "easeInOut";
    result["animation"] = animation;

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
// 消息 handler 注册（阶段 1.7 C1）
//
// 注册 4 个 handler 到全局 router。
// gui handler 均通过 g_pipe 全局直接访问管道，不使用 ctx 参数。
//
// 注：这些 handler 在 Qt 主线程中执行（由 pollPipe 通过 QTimer 触发），
//     可安全访问 Qt 对象（g_pluginLoader / g_statusProvider / g_sessionManager）。
// ================================================================
void registerMessageHandlers(MessageRouter& router) {
    // ---- SHUTDOWN ----
    // 依据 DREAM_MACHINE_SHUTDOWN_COORDINATION 裁决 Q4：
    //   直接 QApplication::quit()，不弹确认框
    //   reason 仅用于日志区分，不影响退出行为
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

    // ---- INIT_LIST ----
    router.register_handler(msg_types::INIT_LIST,
        [](const std::string& payload, void* /*ctx*/) {
            handleInitList(payload);
        });

    // ---- SESSION_STATE_UPDATE ----
    router.register_handler(msg_types::SESSION_STATE_UPDATE,
        [](const std::string& payload, void* /*ctx*/) {
            handleSessionStateUpdate(payload);
        });

    // ---- INIT_SESSION_LIST ----
    router.register_handler(msg_types::INIT_SESSION_LIST,
        [](const std::string& payload, void* /*ctx*/) {
            handleInitSessionList(payload);
        });
}

// ================================================================
// 管道轮询（QTimer 50ms 触发）
//
// 阶段 1.7 C1：走 MessageRouter 分发。
// 依据 DREAM_MACHINE_SHUTDOWN_COORDINATION 裁决 Q4-Q7：
//   - Q4: 收到 SHUTDOWN 直接 QApplication::quit()（由 handler 处理）
//   - Q5: 日志由 Logger 内部每次写入即 flush
//   - Q6: pollTimer 由 QApplication::exec() 返回后的 main() 统一停止
//   - Q7: readLine(3000) 保持不动
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
                // ---- 走分发表（阶段 1.7 C1） ----
                // gui handler 不需要 ctx（通过 g_pipe 全局直接访问）
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
// 日志生命周期（阶段 1.5 P1-5）：
//   - 启动：setProcessName → archiveLastSessionIfDirty → 开始日志
//   - 退出：最后一条日志 → markCleanExit → return 0
//
// QApplication::exec() 返回代表 Qt 事件循环结束（用户关闭窗口或
// SHUTDOWN 触发），均视为正常退出；exec() 返回值不影响标记写入。
//
// 失败路径（父进程校验、连接 launcher 失败）不写 .clean_exit：
// 它们不是正常会话，下次启动时应被识别为异常退出并归档。
//
// 阶段 1.7 C1：消息分发表接入（4 个 handler 迁移）
// ================================================================
int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("gui");

    const bool archived_prev = Logger::instance().archiveLastSessionIfDirty();

    LOG_INFO("=== Dream Machine GUI starting ===");

    if (archived_prev) {
        LOG_INFO("Previous session logs archived to logs/crashes/");
    }

    // 阶段 1.7 C1：注册消息 handler（必须在 Qt 事件循环启动前）
    // 注：registerMessageHandlers 中的 lambda 不访问 Qt 对象，
    //     仅访问全局 router 与函数指针（handleInitList 等）。
    //     实际访问 Qt 对象发生在 pollPipe 触发时（Qt 主线程内），安全。
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

    // C4 编码 helper（阶段 1.7）
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
    LOG_INFO("QApplication initialized with Fusion style");

    auto statusProvider = std::make_unique<StatusProvider>();
    g_statusProvider = statusProvider.get();

    auto sessionManager = std::make_unique<SessionStateManager>();
    g_sessionManager = sessionManager.get();

    auto engine = std::make_unique<QQmlApplicationEngine>();
    g_engine = engine.get();

    QVariantMap globalParams = loadGlobalParams();
    engine->rootContext()->setContextProperty("globalParams", globalParams);
    engine->rootContext()->setContextProperty("statusProvider", statusProvider.get());
    engine->rootContext()->setContextProperty("sessionManager", sessionManager.get());

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

    LOG_INFO("=== GUI exited with code " + std::to_string(result) + " ===");

    Logger::instance().markCleanExit();

    return 0;
}