// src/gui/theme_manager.h
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace dream_machine::gui {

// ================================================================
// ThemeManager：运行时主题偏好管理（gui 进程内）
//
// 职责：
//   1. 启动时读取系统模板 → 得到可用模式列表 + 默认模式
//   2. 读取用户偏好 → 得到当前模式
//   3. 提供 setMode() 供 QML 调用，写入用户偏好
//   4. 通过 modeChanged 信号通知 QML 更新
//
// 设计约束：
//   - 不重载 QML（切换需重启生效）
//   - 与 main.cpp 的 loadGlobalParams() 独立（后者负责启动时注入）
//   - 无跨进程通信（gui 单进程内使用）
//   - 错误仅本地日志（gui.log）
//
// 用户偏好文件：
//   data/theme_preferences.json
//   {
//     "theme_mode": "dark",
//     "colors": { ... },  ← 保留用户自定义，setMode 不覆盖
//     "fonts": { ... }    ← 同上
//   }
//
// QML 使用：
//   // 读当前模式
//   Text { text: themeManager.currentMode }
//
//   // 切换模式
//   Button {
//       onClicked: {
//           if (themeManager.setMode("dark")) {
//               // 提示用户重启
//           }
//       }
//   }
//
//   // 响应模式变更
//   Connections {
//       target: themeManager
//       function onModeChanged() { ... }
//   }
// ================================================================
class ThemeManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString currentMode READ currentMode NOTIFY modeChanged)

public:
    explicit ThemeManager(QObject* parent = nullptr);
    ~ThemeManager() override;

    // 当前主题模式（"light" / "dark" / ...）
    // QML 只读属性
    [[nodiscard]] QString currentMode() const;

    // 切换主题模式
    // 行为：
    //   1. 校验 mode 是否在 availableModes() 中；不在 → false + WARN
    //   2. mode == current_mode_ → true（幂等）
    //   3. 更新 user_prefs_["theme_mode"]
    //   4. 写回 data/theme_preferences.json（保留其他字段）
    //   5. 更新 current_mode_ + emit modeChanged()
    // 返回：true = 已保存（需重启生效）；false = 失败
    Q_INVOKABLE bool setMode(const QString& mode);

    // 可用模式列表（从系统模板 theme.modes 的键提取）
    // 若系统模板不可读 → 兜底 ["light", "dark"]
    // QML 可用于构建切换菜单
    Q_INVOKABLE QStringList availableModes() const;

signals:
    // 模式变更（仅运行时通知；实际视觉变化需重启）
    void modeChanged();

private:
    QString current_mode_;         // 当前模式
    QStringList available_modes_;  // 可用模式列表
    QVariantMap user_prefs_;       // 用户偏好缓存（setMode 时读、写）

    // 构造时调用：读系统模板 + 用户偏好 → 填充上述三个成员
    void loadFromFiles();

    // 读系统模板：返回 QVariantMap（空表示读不到）
    // 内部处理生产路径 + dev 路径的逻辑
    // 注：不在此处做完整合并——合并由 main.cpp 的 loadGlobalParams 负责
    [[nodiscard]] QVariantMap readSystemTemplate() const;

    // 从系统模板中提取可用模式列表
    [[nodiscard]] QStringList extractModesFromTemplate(const QVariantMap& sys_template) const;

    // 从系统模板中提取 default_mode
    [[nodiscard]] QString extractDefaultMode(const QVariantMap& sys_template) const;
};

} // namespace dream_machine::gui