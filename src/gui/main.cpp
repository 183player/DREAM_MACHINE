// src/gui/main.cpp
#include "logger.h"
#include "pipe.h"
#include "constants.h"
#include "messages.h"
#include "session_state_manager.h"
#include "plugin_loader.h"
#include "status_provider.h"

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>
#include <QUrl>
#include <QObject>
#include <QDebug>
#include <QFileInfo>
#include <QFile>
#include <QString>
#include <QCoreApplication>
#include <QDir>
#include <QQuickWindow>

#include <string>
#include <cstdlib>
#include <tlhelp32.h>

using namespace dream_machine;
using namespace dream_machine::gui;

namespace {

NamedPipe* g_pipe = nullptr;
SessionStateManager* g_sessionManager = nullptr;
QQmlApplicationEngine* g_engine = nullptr;
PluginLoader* g_pluginLoader = nullptr;
StatusProvider* g_statusProvider = nullptr;
QObject* g_placeholderWindow = nullptr;
QTimer* g_timeoutTimer = nullptr;
QTimer* g_showPlaceholderTimer = nullptr;   // 延迟显示占位窗口

bool isDevMode() {
    const char* dev_mode = std::getenv("DM_DEV_MODE");
    return dev_mode && std::string(dev_mode) == "1";
}

std::string getArgValue(int argc, char* argv[], const std::string& key) {
    for (int i = 1; i < argc - 1; ++i) {
        if (argv[i] == key) {
            return argv[i + 1];
        }
    }
    return {};
}

bool verifyParentPid(DWORD expected_parent_pid) {
    if (expected_parent_pid == 0) {
        LOG_ERROR("Missing --parent-pid argument, refusing to run standalone");
        return false;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        LOG_ERROR("CreateToolhelp32Snapshot failed: error " + std::to_string(GetLastError()));
        return false;
    }

    PROCESSENTRY32W pe = {sizeof(PROCESSENTRY32W)};
    DWORD current_pid = GetCurrentProcessId();
    DWORD real_parent_pid = 0;

    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (pe.th32ProcessID == current_pid) {
                real_parent_pid = pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &pe));
    }

    CloseHandle(snapshot);

    if (real_parent_pid == 0) {
        LOG_ERROR("Failed to determine real parent PID");
        return false;
    }

    if (real_parent_pid != expected_parent_pid) {
        LOG_ERROR("Parent PID mismatch: expected " + std::to_string(expected_parent_pid) +
                  ", actual " + std::to_string(real_parent_pid) + ", refusing to run");
        return false;
    }

    LOG_INFO("Parent PID verification passed (PID: " + std::to_string(real_parent_pid) + ")");
    return true;
}

// ----- 显示占位窗口（由定时器触发） -----
void showPlaceholderWindow() {
    if (g_placeholderWindow) {
        QQuickWindow* win = qobject_cast<QQuickWindow*>(g_placeholderWindow);
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

// ----- 隐藏或删除占位窗口（成功加载时调用） -----
void hidePlaceholderWindow() {
    if (g_showPlaceholderTimer) {
        g_showPlaceholderTimer->stop();
    }
    if (g_placeholderWindow) {
        g_placeholderWindow->deleteLater();
        g_placeholderWindow = nullptr;
        LOG_INFO("Placeholder window destroyed");
    }
}

// ----- 处理 INIT_LIST 消息 -----
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

        // 取消延迟显示定时器，隐藏占位窗口
        hidePlaceholderWindow();

        // 如果框架已加载，发送 ACK
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

        // 取消延迟显示定时器，立即显示占位窗口（如果尚未显示）
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

void handleInitSessionList(const std::string& /*message*/) {
    if (!g_sessionManager) {
        return;
    }
    g_sessionManager->clearAll();
    LOG_INFO("Initialized session list (empty)");
}

void handleSessionStateUpdate(const std::string& message) {
    if (!g_sessionManager) {
        return;
    }

    std::string session_id;
    std::string state;

    size_t id_pos = message.find("\"session_id\"");
    if (id_pos != std::string::npos) {
        size_t start = message.find('\"', id_pos + 14);
        if (start != std::string::npos) {
            size_t end = message.find('\"', start + 1);
            if (end != std::string::npos) {
                session_id = message.substr(start + 1, end - start - 1);
            }
        }
    }

    size_t state_pos = message.find("\"state\"");
    if (state_pos != std::string::npos) {
        size_t start = message.find('\"', state_pos + 8);
        if (start != std::string::npos) {
            size_t end = message.find('\"', start + 1);
            if (end != std::string::npos) {
                state = message.substr(start + 1, end - start - 1);
            }
        }
    }

    if (session_id.empty() || state.empty()) {
        LOG_WARN("Invalid SESSION_STATE_UPDATE message");
        return;
    }

    LOG_INFO("Session state update: " + session_id + " -> " + state);
    g_sessionManager->updateSessionState(session_id, state);
}

// 全局超时处理（10秒）
void onTimeout() {
    // 取消延迟显示定时器
    if (g_showPlaceholderTimer) {
        g_showPlaceholderTimer->stop();
    }

    // 显示占位窗口
    showPlaceholderWindow();

    if (g_statusProvider) {
        g_statusProvider->setStatusText("加载超时");
        g_statusProvider->setErrorText("未收到 launcher 的插件列表，请检查 launcher 是否正常运行");
        g_statusProvider->setLoading(false);
        g_statusProvider->setShowExitButton(true);
    }
    LOG_ERROR("Timeout waiting for INIT_LIST");
}

// ================================================================
// 管道轮询
// ================================================================

void pollPipe() {
    if (!g_pipe) {
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
                if (type == msg_types::INIT_LIST) {
                    handleInitList(payload);
                } else if (type == msg_types::SESSION_STATE_UPDATE) {
                    handleSessionStateUpdate(payload);
                } else if (type == msg_types::INIT_SESSION_LIST) {
                    handleInitSessionList(payload);
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

} // namespace

// ================================================================
// main 入口
// ================================================================
int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("gui");
    LOG_INFO("=== Dream Machine GUI starting ===");

    std::string parent_pid_str = getArgValue(argc, argv, "--parent-pid");
    DWORD expected_parent_pid = 0;
    if (!parent_pid_str.empty()) {
        expected_parent_pid = static_cast<DWORD>(std::stoul(parent_pid_str));
    }

    if (!verifyParentPid(expected_parent_pid)) {
        return 1;
    }

    std::string pipe_name_str = pipe_names::launcher_gui();
    std::wstring pipe_name(pipe_name_str.begin(), pipe_name_str.end());

    LOG_INFO("Connecting to launcher pipe: " + pipe_name_str);

    NamedPipe pipe;
    if (!pipe.connect(pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to launcher pipe, exiting");
        return 1;
    }

    LOG_INFO("Connected to launcher pipe");

    RegisterMessage reg_msg;
    reg_msg.process = "gui";
    std::string register_msg = serializeRegister(reg_msg);

    if (pipe.writeLine(register_msg) != PipeResult::PIPE_OK) {
        LOG_ERROR("Failed to send registration message");
    } else {
        LOG_INFO("Registration message sent: " + register_msg);
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName("Dream Machine");
    QApplication::setOrganizationName("DreamMachine");

    LOG_INFO("QApplication initialized");

    // ----- 创建状态提供者 -----
    StatusProvider statusProvider;
    g_statusProvider = &statusProvider;

    // ----- 初始化会话状态管理器 -----
    SessionStateManager sessionManager;
    g_sessionManager = &sessionManager;

    LOG_INFO("Creating QQmlApplicationEngine...");
    QQmlApplicationEngine engine;
    g_engine = &engine;

    engine.rootContext()->setContextProperty("statusProvider", &statusProvider);
    engine.rootContext()->setContextProperty("sessionManager", &sessionManager);

    QObject::connect(&engine, &QQmlApplicationEngine::warnings,
        [](const QList<QQmlError>& warnings) {
            for (const auto& error : warnings) {
                LOG_ERROR("QML warning: " + error.toString().toStdString());
            }
        });

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
        [](QObject* obj, const QUrl& objUrl) {
            if (obj) {
                LOG_INFO("QML object created for: " + objUrl.toString().toStdString());
            } else {
                LOG_ERROR("QML object creation failed for: " + objUrl.toString().toStdString());
            }
        });

    // ----- 初始化插件加载器 -----
    PluginLoader pluginLoader;
    g_pluginLoader = &pluginLoader;
    pluginLoader.setEngine(&engine);

    bool dev_mode = isDevMode();
    if (dev_mode) {
        LOG_INFO("=== DEVELOPMENT MODE ENABLED ===");
        statusProvider.setStatusText("开发模式 - 从源码加载");
    }

    // ----- 加载占位窗口（默认不可见） -----
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
            engine.load(url);
            LOG_INFO("Development placeholder loaded from: " + placeholder_path.toStdString());
        } else {
            LOG_WARN("Development placeholder not found, trying build directory");
            placeholder_path = QDir(app_dir).filePath("src/gui/qml/main.qml");
            engine.load(QUrl::fromLocalFile(placeholder_path));
        }
    } else {
        QDir base_dir(app_dir);
        bool up1 = base_dir.cdUp();
        bool up2 = base_dir.cdUp();
        if (up1 && up2) {
            placeholder_path = base_dir.filePath("src/gui/qml/main.qml");
        } else {
            placeholder_path = app_dir + "/../src/gui/qml/main.qml";
        }
        QUrl url = QUrl::fromLocalFile(placeholder_path);
        if (QFile::exists(placeholder_path)) {
            engine.load(url);
            LOG_INFO("Placeholder loaded from: " + placeholder_path.toStdString());
        } else {
            LOG_ERROR("Placeholder QML not found at: " + placeholder_path.toStdString());
            engine.load(QUrl::fromLocalFile("../src/gui/qml/main.qml"));
        }
    }

    if (!engine.rootObjects().isEmpty()) {
        g_placeholderWindow = engine.rootObjects().first();
        g_placeholderWindow->setObjectName("placeholder");
        // 初始不可见（QML 中 visible: false）
        LOG_INFO("Placeholder window created (initially hidden)");
    }

    // ----- 延迟显示定时器（500ms） -----
    QTimer showTimer;
    g_showPlaceholderTimer = &showTimer;
    showTimer.setSingleShot(true);
    showTimer.setInterval(500);
    QObject::connect(&showTimer, &QTimer::timeout, showPlaceholderWindow);
    showTimer.start();
    LOG_INFO("Placeholder show timer started (500ms)");

    // ----- 全局超时定时器（10秒） -----
    QTimer timeoutTimer;
    g_timeoutTimer = &timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval(10000);
    QObject::connect(&timeoutTimer, &QTimer::timeout, onTimeout);
    timeoutTimer.start();

    // ----- 设置管道轮询 -----
    g_pipe = &pipe;
    QTimer poll_timer;
    poll_timer.setInterval(50);
    QObject::connect(&poll_timer, &QTimer::timeout, pollPipe);
    poll_timer.start();

    // ----- 进入 Qt 事件循环 -----
    LOG_INFO("Entering Qt event loop...");
    int result = QApplication::exec();

    // ----- 清理 -----
    LOG_INFO("Shutting down GUI...");
    poll_timer.stop();
    timeoutTimer.stop();
    showTimer.stop();

    pluginLoader.reset();
    g_pluginLoader = nullptr;

    g_pipe = nullptr;
    g_sessionManager = nullptr;
    g_engine = nullptr;
    g_statusProvider = nullptr;
    g_placeholderWindow = nullptr;
    g_timeoutTimer = nullptr;
    g_showPlaceholderTimer = nullptr;
    pipe.close();

    LOG_INFO("=== GUI exited with code " + std::to_string(result) + " ===");
    return result;
}