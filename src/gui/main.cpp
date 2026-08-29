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

    if (!g_pipe->isValid()) {
        LOG_WARN("Pipe is invalid");
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

int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("gui");
    LOG_INFO("=== Dream Machine GUI starting ===");

    // 1. 解析命令行参数，获取父进程传递的管道句柄
    uintptr_t handle_value = 0;
    if (!parsePipeHandleFromArgs(argc, argv, handle_value)) {
        LOG_ERROR("No --pipe-handle provided, cannot connect to launcher");
        return 1;
    }

    LOG_INFO("Received --pipe-handle: " + std::to_string(handle_value));

    // 2. 接管父进程传递的管道句柄
    NamedPipe pipe = NamedPipe::adopt(handle_value);
    if (!pipe.isValid()) {
        LOG_ERROR("Failed to adopt pipe handle: " + std::to_string(handle_value));
        return 1;
    }

    LOG_INFO("Successfully adopted pipe handle: " + std::to_string(handle_value));

    // 3. 发送注册消息
    std::string register_msg = R"({"type":"register","process":"gui"})";
    if (pipe.writeLine(register_msg) != PipeResult::OK) {
        LOG_ERROR("Failed to send registration message to launcher");
        // 继续运行，允许 GUI 在离线模式下启动
    } else {
        LOG_INFO("Registration message sent to launcher: " + register_msg);
    }

    // 4. 初始化 Qt 应用
    QApplication app(argc, argv);
    QApplication::setApplicationName("Dream Machine");
    QApplication::setOrganizationName("DreamMachine");

    LOG_INFO("QApplication initialized");

    // 5. 设置管道轮询（使用 QTimer 非阻塞轮询）
    g_pipe = &pipe;
    QTimer poll_timer;
    poll_timer.setInterval(50);
    QObject::connect(&poll_timer, &QTimer::timeout, pollPipe);
    poll_timer.start();

    // 6. 创建 QML 引擎
    LOG_INFO("Creating QQmlApplicationEngine...");
    QQmlApplicationEngine engine;

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

    const QUrl qml_url = QUrl::fromLocalFile("../src/gui/qml/main.qml");
    LOG_INFO("Loading QML from: " + qml_url.toString().toStdString());

    QString local_path = qml_url.toLocalFile();
    QFileInfo file_info(local_path);
    if (file_info.exists()) {
        LOG_INFO("QML file exists: " + local_path.toStdString());
        LOG_INFO("File size: " + std::to_string(file_info.size()));
    } else {
        LOG_ERROR("QML file NOT FOUND: " + local_path.toStdString());

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

    // 7. 进入 Qt 事件循环
    LOG_INFO("Entering Qt event loop...");
    int result = QApplication::exec();

    // 8. 清理
    LOG_INFO("Shutting down GUI...");
    poll_timer.stop();
    g_pipe = nullptr;
    pipe.close();

    LOG_INFO("=== GUI exited with code " + std::to_string(result) + " ===");
    return result;
}