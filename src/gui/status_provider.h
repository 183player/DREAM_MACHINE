// src/gui/status_provider.h
#pragma once

#include <QObject>
#include <QString>
#include <QApplication>   // 需要 QApplication::quit()

// ================================================================
// 状态提供者（暴露给 QML，用于占位窗口显示加载状态）
// ================================================================
class StatusProvider : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString statusText READ statusText WRITE setStatusText NOTIFY statusChanged)
    Q_PROPERTY(QString errorText READ errorText WRITE setErrorText NOTIFY statusChanged)
    Q_PROPERTY(bool loading READ loading WRITE setLoading NOTIFY statusChanged)
    Q_PROPERTY(bool showExitButton READ showExitButton WRITE setShowExitButton NOTIFY statusChanged)

public:
    explicit StatusProvider(QObject* parent = nullptr) : QObject(parent) {}

    QString statusText() const { return m_statusText; }
    void setStatusText(const QString& text) {
        if (m_statusText != text) {
            m_statusText = text;
            emit statusChanged();
        }
    }

    QString errorText() const { return m_errorText; }
    void setErrorText(const QString& text) {
        if (m_errorText != text) {
            m_errorText = text;
            emit statusChanged();
        }
    }

    bool loading() const { return m_loading; }
    void setLoading(bool loading) {
        if (m_loading != loading) {
            m_loading = loading;
            emit statusChanged();
        }
    }

    bool showExitButton() const { return m_showExitButton; }
    void setShowExitButton(bool show) {
        if (m_showExitButton != show) {
            m_showExitButton = show;
            emit statusChanged();
        }
    }

    Q_INVOKABLE void exitApp() {
        QApplication::quit();
    }

    signals:
        void statusChanged();

private:
    QString m_statusText = "正在加载插件...";
    QString m_errorText;
    bool m_loading = true;
    bool m_showExitButton = false;
};