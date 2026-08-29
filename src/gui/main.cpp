// src/gui/main.cpp
#include "logger.h"
#include "pipe.h"
#include "constants.h"

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QTimer>
#include <QUrl>
#include <QObject>
#include <QDebug>
#include <QFileInfo>
#include <QFile>
#include <QString>
#include <QCoreApplication>
#include <QDir>

#include <string>
#include <tlhelp32.h>

using namespace dream_machine;

// ================================================================
// 内部链接限制（匿名命名空间）
// ================================================================
namespace {

// 全局管道指针（内部链接）
NamedPipe* g_pipe = nullptr;

// 命令行参数解析
std::string getArgValue(int argc, char* argv[], const std::string& key) {
    for (int i = 1; i < argc - 1; ++i) {
        if (argv[i] == key) {
            return argv[i + 1];
        }
    }
    return {};
}

// 验证父进程 PID，防止独立启动
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

// 管道轮询函数（内部链接）
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
        PipeResult read_result = g_pipe->readLine(message, 100);

        if (read_result == PipeResult::PIPE_OK) {
            LOG_INFO("Received: " + message);
        } else if (read_result == PipeResult::PIPE_BROKEN) {
            LOG_WARN("Pipe broken (read), quitting GUI");
            QApplication::quit();
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

    // ============================================================
    // 验证启动凭证（防止独立启动）
    // ============================================================
    std::string parent_pid_str = getArgValue(argc, argv, "--parent-pid");
    DWORD expected_parent_pid = 0;
    if (!parent_pid_str.empty()) {
        expected_parent_pid = static_cast<DWORD>(std::stoul(parent_pid_str));
    }

    if (!verifyParentPid(expected_parent_pid)) {
        return 1;
    }

    // ============================================================
    // 连接到 launcher（直接连接，无需重试）
    // ============================================================
    std::string pipe_name_str = pipe_names::launcher_gui();
    std::wstring pipe_name(pipe_name_str.begin(), pipe_name_str.end());

    LOG_INFO("Connecting to launcher pipe: " + pipe_name_str);

    NamedPipe pipe;
    if (!pipe.connect(pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to launcher pipe, exiting");
        return 1;
    }

    LOG_INFO("Connected to launcher pipe");

    // ============================================================
    // 发送注册消息
    // ============================================================
    std::string register_msg = R"({"type":"register","process":"gui"})";
    if (pipe.writeLine(register_msg) != PipeResult::PIPE_OK) {
        LOG_ERROR("Failed to send registration message");
        // 继续运行，允许离线模式
    } else {
        LOG_INFO("Registration message sent: " + register_msg);
    }

    // ============================================================
    // 初始化 Qt 应用
    // ============================================================
    QApplication app(argc, argv);
    QApplication::setApplicationName("Dream Machine");
    QApplication::setOrganizationName("DreamMachine");

    LOG_INFO("QApplication initialized");

    // ============================================================
    // 管道轮询定时器
    // ============================================================
    g_pipe = &pipe;
    QTimer poll_timer;
    poll_timer.setInterval(50);
    QObject::connect(&poll_timer, &QTimer::timeout, pollPipe);
    poll_timer.start();

    // ============================================================
    // QML 引擎（使用基于可执行文件路径的绝对寻址）
    // ============================================================
    LOG_INFO("Creating QQmlApplicationEngine...");
    QQmlApplicationEngine engine;

    QObject::connect(&engine, &QQmlApplicationEngine::warnings,
        [](const QList<QQmlError>& warnings) {
            for (const auto& error : warnings) {
                LOG_ERROR("QML warning: " + error.toString().toStdString());
            }
        });

    // 注意：obj 不能改为 const QObject*，必须与信号签名精确匹配
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
        [](QObject* obj, const QUrl& objUrl) {
            if (obj) {
                LOG_INFO("QML object created for: " + objUrl.toString().toStdString());
            } else {
                LOG_ERROR("QML object creation failed for: " + objUrl.toString().toStdString());
            }
        });

    // ----- 路径解析核心逻辑 -----
    QString app_dir = QCoreApplication::applicationDirPath();
    LOG_INFO("Application directory: " + app_dir.toStdString());

    QDir base_dir(app_dir);
    bool up1 = base_dir.cdUp();
    bool up2 = base_dir.cdUp();

    if (!up1 || !up2) {
        LOG_ERROR("Failed to navigate up from application directory: " + app_dir.toStdString());
        LOG_ERROR("Attempting fallback to relative path...");
        const QUrl fallback_url = QUrl::fromLocalFile("../src/gui/qml/main.qml");
        engine.load(fallback_url);
        if (engine.rootObjects().isEmpty()) {
            LOG_ERROR("Fallback QML load also failed, exiting.");
            return 1;
        }
        LOG_INFO("Fallback QML load succeeded.");
    } else {
        QString qml_absolute_path = base_dir.filePath("src/gui/qml/main.qml");
        QUrl qml_url = QUrl::fromLocalFile(qml_absolute_path);
        LOG_INFO("Loading QML from (absolute): " + qml_absolute_path.toStdString());

        QFileInfo file_info(qml_absolute_path);
        if (file_info.exists()) {
            LOG_INFO("QML file exists: " + qml_absolute_path.toStdString());
            LOG_INFO("File size: " + std::to_string(file_info.size()));
        } else {
            LOG_ERROR("QML file NOT FOUND at: " + qml_absolute_path.toStdString());
            const QUrl fallback_url = QUrl::fromLocalFile("../src/gui/qml/main.qml");
            LOG_INFO("Attempting fallback relative path...");
            engine.load(fallback_url);
            if (engine.rootObjects().isEmpty()) {
                LOG_ERROR("Fallback QML load failed, exiting.");
                return 1;
            }
            LOG_INFO("Fallback QML load succeeded.");
            goto after_load;
        }

        engine.load(qml_url);
    }

after_load:

    if (engine.rootObjects().isEmpty()) {
        LOG_ERROR("QML engine loaded but NO ROOT OBJECTS created");
        LOG_INFO("Available import paths:");
        for (const auto& path : engine.importPathList()) {
            LOG_INFO("  " + path.toStdString());
        }
    } else {
        LOG_INFO("QML engine created " + std::to_string(engine.rootObjects().size()) + " root objects");
        for (auto* obj : engine.rootObjects()) {
            LOG_INFO("  Root object: " + std::string(obj->metaObject()->className()));
        }
    }

    // ============================================================
    // 进入 Qt 事件循环
    // ============================================================
    LOG_INFO("Entering Qt event loop...");
    int result = QApplication::exec();

    // ============================================================
    // 清理
    // ============================================================
    LOG_INFO("Shutting down GUI...");
    poll_timer.stop();
    g_pipe = nullptr;
    pipe.close();

    LOG_INFO("=== GUI exited with code " + std::to_string(result) + " ===");
    return result;
}