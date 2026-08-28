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

#include <string>
#include <cstdlib>

using namespace dream_machine;

namespace {

bool parsePipeHandleFromArgs(int argc, char* argv[], uintptr_t& out_handle) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        const std::string prefix = "--pipe-handle=";
        if (arg.find(prefix) == 0) {
            std::string value_str = arg.substr(prefix.length());
            try {
                out_handle = std::stoull(value_str);
                return true;
            } catch (const std::exception&) {
                LOG_ERROR("Failed to parse --pipe-handle value: " + value_str);
                return false;
            }
        }
    }
    return false;
}

NamedPipe* g_pipe = nullptr;

void pollPipe() {
    if (!g_pipe) {
        return;
    }

    DWORD bytes_available = 0;
    PipeResult peek_result = g_pipe->peekAvailable(bytes_available);

    if (peek_result == PipeResult::OK && bytes_available > 0) {
        std::string message;
        PipeResult read_result = g_pipe->readLine(message, 100);

        if (read_result == PipeResult::OK) {
            LOG_INFO("Received: " + message);
        } else if (read_result == PipeResult::BROKEN) {
            LOG_WARN("Pipe broken");
            QApplication::quit();
        }
    } else if (peek_result == PipeResult::BROKEN) {
        LOG_WARN("Pipe broken");
        QApplication::quit();
    }
}

} // namespace

// ============================================================================
// 主入口
// ============================================================================

int main(int argc, char* argv[]) {
    // 1. 初始化日志
    Logger::instance().setProcessName("gui");
    LOG_INFO("=== Dream Machine GUI starting ===");

    // 2. 解析命令行参数
    uintptr_t pipe_handle_value = 0;
    if (parsePipeHandleFromArgs(argc, argv, pipe_handle_value)) {
        LOG_INFO("Received --pipe-handle: " + std::to_string(pipe_handle_value));
    } else {
        LOG_INFO("No --pipe-handle provided, connecting via pipe name");
    }

    // 3. 初始化 Qt 应用
    QApplication app(argc, argv);
    QApplication::setApplicationName("Dream Machine");
    QApplication::setOrganizationName("DreamMachine");

    LOG_INFO("QApplication initialized");

    // 4. 连接到 launcher 管道（如果 launcher 未运行，此步骤会失败）
    std::wstring pipe_name = L"\\\\.\\pipe\\DreamMachine_Launcher";
    NamedPipe pipe;

    LOG_INFO("Connecting to launcher pipe: " + std::string(pipe_name.begin(), pipe_name.end()));

    if (!pipe.connect(pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to launcher pipe");
        // 不退出，继续测试 QML 加载
    } else {
        LOG_INFO("Connected to launcher pipe");

        std::string register_msg = R"({"type":"register","process":"gui"})";
        if (pipe.writeLine(register_msg) != PipeResult::OK) {
            LOG_ERROR("Failed to send registration message");
        } else {
            LOG_INFO("Registration message sent: " + register_msg);
        }

        g_pipe = &pipe;
        QTimer poll_timer;
        poll_timer.setInterval(50);
        QObject::connect(&poll_timer, &QTimer::timeout, pollPipe);
        poll_timer.start();
    }

    // 5. 创建 QML 引擎
    LOG_INFO("Creating QQmlApplicationEngine...");
    QQmlApplicationEngine engine;

    // 连接 QML 引擎的警告信号
    QObject::connect(&engine, &QQmlApplicationEngine::warnings,
        [](const QList<QQmlError>& warnings) {
            for (const auto& error : warnings) {
                LOG_ERROR("QML warning: " + error.toString().toStdString());
            }
        });

    // 连接 QML 引擎的加载完成信号
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
        [](const QObject* obj, const QUrl& objUrl) {
            if (obj) {
                LOG_INFO("QML object created for: " + objUrl.toString().toStdString());
            } else {
                LOG_ERROR("QML object creation failed for: " + objUrl.toString().toStdString());
            }
        });

    // 加载 QML
    const QUrl qml_url = QUrl::fromLocalFile("../src/gui/qml/main.qml");
    LOG_INFO("Loading QML from: " + qml_url.toString().toStdString());

    // 检查文件是否存在
    QString local_path = qml_url.toLocalFile();
    QFileInfo file_info(local_path);
    if (file_info.exists()) {
        LOG_INFO("QML file exists: " + local_path.toStdString());
        LOG_INFO("File size: " + std::to_string(file_info.size()));
    } else {
        LOG_ERROR("QML file NOT FOUND: " + local_path.toStdString());

        // 尝试绝对路径
        QString abs_path = "E:/Dream_machine_v3.0(alpha)/src/gui/qml/main.qml";
        LOG_INFO("Trying absolute path: " + abs_path.toStdString());
        QFileInfo abs_info(abs_path);
        if (abs_info.exists()) {
            LOG_INFO("Absolute path file exists");
            engine.load(QUrl::fromLocalFile(abs_path));
        } else {
            LOG_ERROR("Absolute path file also not found");
        }
    }

    engine.load(qml_url);

    // 检查根对象
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

    // 6. 进入 Qt 事件循环
    LOG_INFO("Entering Qt event loop...");
    int result = QApplication::exec();

    // 7. 清理
    LOG_INFO("Shutting down GUI...");
    if (g_pipe) {
        g_pipe = nullptr;
    }
    pipe.close();

    LOG_INFO("=== GUI exited with code " + std::to_string(result) + " ===");
    return result;
}