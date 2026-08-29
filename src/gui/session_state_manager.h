// src/gui/session_state_manager.h
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <string>
#include <unordered_map>
#include <mutex>

// ================================================================
// 会话状态管理器（暴露给 QML）
// ================================================================
class SessionStateManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(int sessionCount READ sessionCount NOTIFY sessionCountChanged)

public:
    explicit SessionStateManager(QObject* parent = nullptr) : QObject(parent) {}

    // QML 可调用接口
    Q_INVOKABLE bool isSessionRunning(const QString& sessionId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = states_.find(sessionId.toStdString());
        if (it == states_.end()) {
            return false;
        }
        return it->second == "running";
    }

    Q_INVOKABLE QString getSessionState(const QString& sessionId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = states_.find(sessionId.toStdString());
        if (it == states_.end()) {
            return "unknown";
        }
        return QString::fromStdString(it->second);
    }

    Q_INVOKABLE QStringList getRunningSessions() const {
        std::lock_guard<std::mutex> lock(mutex_);
        QStringList result;
        for (const auto& pair : states_) {
            if (pair.second == "running") {
                result.append(QString::fromStdString(pair.first));
            }
        }
        return result;
    }

    // 内部接口（由 C++ 调用）
    void updateSessionState(const std::string& sessionId, const std::string& state) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = states_.find(sessionId);
            if (it == states_.end()) {
                states_[sessionId] = state;
                changed = true;
            } else if (it->second != state) {
                it->second = state;
                changed = true;
            }
        }
        if (changed) {
            emit sessionStateChanged(QString::fromStdString(sessionId),
                                     QString::fromStdString(state));
            emit sessionCountChanged();
        }
    }

    void removeSession(const std::string& sessionId) {
        bool existed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = states_.find(sessionId);
            if (it != states_.end()) {
                states_.erase(it);
                existed = true;
            }
        }
        if (existed) {
            emit sessionRemoved(QString::fromStdString(sessionId));
            emit sessionCountChanged();
        }
    }

    void clearAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        states_.clear();
        emit sessionCountChanged();
    }

    int sessionCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(states_.size());
    }

signals:
    void sessionStateChanged(const QString& sessionId, const QString& state);
    void sessionRemoved(const QString& sessionId);
    void sessionCountChanged();

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> states_;
};