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

using namespace dream_machine;

NamedPipe* g_pipe = nullptr;

void pollPipe() {
    if (!g_pipe) {
        return;
    }

    if (!g_pipe->isValid()) {
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

int main(int argc, char* argv[]) {
    Logger::instance().setProcessName("gui");
    LOG_INFO("=== Dream Machine GUI starting ===");

    // 通过名称连接 launcher
    std::string pipe_name_str = pipe_names::launcher_gui();
    std::wstring pipe_name(pipe_name_str.begin(), pipe_name_str.end());

    LOG_INFO("Connecting to launcher pipe: " + pipe_name_str);

    NamedPipe pipe;
    if (!pipe.connect(pipe_name, 5000)) {
        LOG_ERROR("Failed to connect to launcher pipe");
        return 1;
    }

    LOG_INFO("Connected to launcher pipe");

    // 发送注册消息
    std::string register_msg = R"({"type":"register","process":"gui"})";
    if (pipe.writeLine(register_msg) != PipeResult::OK) {
        LOG_ERROR("Failed to send registration message");
        // 继续运行，允许离线模式
    } else {
        LOG_INFO("Registration message sent: " + register_msg);
    }

    // 初始化 Qt 应用
    QApplication app(argc, argv);
    QApplication::setApplicationName("Dream Machine");
    QApplication::setOrganizationName("DreamMachine");

    LOG_INFO("QApplication initialized");

    // 管道轮询
    g_pipe = &pipe;
    QTimer poll_timer;
    poll_timer.setInterval(50);
    QObject::connect(&poll_timer, &QTimer::timeout, pollPipe);
    poll_timer.start();

    // QML 引擎
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

    LOG_INFO("Entering Qt event loop...");
    int result = QApplication::exec();

    LOG_INFO("Shutting down GUI...");
    poll_timer.stop();
    g_pipe = nullptr;
    pipe.close();

    LOG_INFO("=== GUI exited with code " + std::to_string(result) + " ===");
    return result;
}