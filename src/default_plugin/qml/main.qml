// src/default_plugin/qml/main.qml
// Dream Machine 默认框架主窗口
//
// 容器 ID（用于插件扩展）：
//   - sessionListContainer   : 会话列表区域
//   - chatViewContainer      : 对话流区域
//   - inputAreaContainer     : 输入区域（含输入框和发送按钮）
//   - statusBarContainer     : 状态栏区域
//
// 窗口尺寸：
//   - 初始尺寸：启动时基于屏幕分辨率计算（C++ 注入 initial_window_*）
//   - 最小尺寸：用户可自由拉伸，但不可小于 min_window_*

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 导入组件子目录，使 ChatView、SessionList 等类型可用
import "components"
// import "cards"

ApplicationWindow {
    id: rootWindow

    // ---------- 从全局参数读取配置 ----------
    readonly property var g: globalParams  // 由 C++ 注入

    // 初始尺寸（基于屏幕分辨率计算；C++ 注入）
    width: g.layout.initial_window_width
    height: g.layout.initial_window_height

    // 最小尺寸（用户无法拉伸到比这更小）
    minimumWidth: g.layout.min_window_width
    minimumHeight: g.layout.min_window_height

    title: "Dream Machine"
    visible: true
    color: g.colors.background

    // ---------- 主布局 ----------
    RowLayout {
        anchors.fill: parent
        anchors.margins: 0
        spacing: 0

        // ----- 左列：会话列表（固定宽度） -----
        Rectangle {
            id: sessionListContainer
            Layout.fillHeight: true
            Layout.preferredWidth: g.layout.session_list_width
            Layout.minimumWidth: 120
            color: g.colors.background_alt
            border.color: g.colors.border
            border.width: 1

            SessionList {
                anchors.fill: parent
                anchors.margins: g.spacing.padding_normal
                // 可在此绑定信号等
            }
        }

        // ----- 右列：主区域（对话流 + 输入区 + 状态栏） -----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // 对话流区域
            Rectangle {
                id: chatViewContainer
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: g.colors.surface
                border.color: g.colors.border
                border.width: 1

                ChatView {
                    anchors.fill: parent
                    anchors.margins: g.spacing.padding_normal
                }
            }

            // 输入区域（输入框 + 发送按钮）
            Rectangle {
                id: inputAreaContainer
                Layout.fillWidth: true
                Layout.preferredHeight: g.layout.input_area_height
                color: g.colors.surface_alt
                border.color: g.colors.border
                border.width: 1

                InputBar {
                    anchors.fill: parent
                    anchors.margins: g.spacing.padding_normal
                }
            }

            // 状态栏
            Rectangle {
                id: statusBarContainer
                Layout.fillWidth: true
                Layout.preferredHeight: g.layout.status_bar_height
                color: g.colors.background_alt
                border.color: g.colors.border
                border.width: 1

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: g.spacing.padding_small
                    spacing: 8

                    Text {
                        text: "轮数: 0"
                        color: g.colors.text_secondary
                        font.pointSize: g.fonts.size_small
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        text: "状态: 就绪"
                        color: g.colors.text_secondary
                        font.pointSize: g.fonts.size_small
                    }

                    // ----------------------------------------------------------------
                    // 主题切换按钮（临时）
                    //
                    // 用途：主题系统（浅色 / 深色）的功能验证
                    // 计划：未来移入"设置"页面的通用项
                    // 移除此处后，ThemeManager 接口（C++ 侧）保持不变
                    //
                    // 行为：
                    //   1. 显示当前模式（浅色 / 深色）
                    //   2. 点击切换到另一模式
                    //   3. 弹窗提示"重启生效"
                    // ----------------------------------------------------------------
                    Button {
                        id: themeToggleButton

                        enabled: (typeof themeManager !== "undefined") &&
                            themeManager !== null

                        text: {
                            if (!enabled) return "主题: 未知"
                            return (themeManager.currentMode === "light")
                                ? "主题: 浅色"
                                : "主题: 深色"
                        }

                        flat: true
                        font.pointSize: g.fonts.size_small
                        padding: 0

                        Layout.fillHeight: true
                        Layout.preferredWidth: implicitContentWidth + 12

                        onClicked: {
                            if (!themeManager) return

                            var next = (themeManager.currentMode === "light")
                                ? "dark"
                                : "light"

                            if (themeManager.setMode(next)) {
                                themeRestartDialog.open()
                            }
                        }
                    }
                }
            }
        }
    }

    // ----------------------------------------------------------------
    // 主题切换提示弹窗（临时）
    //
    // 用标准 Dialog（Fusion 样式），不做主题跟随——保持代码简单。
    // 未来按钮移入"设置"后，本弹窗一并移除。
    // ----------------------------------------------------------------
    Dialog {
        id: themeRestartDialog

        anchors.centerIn: Overlay.overlay
        modal: true
        title: "主题已保存"
        standardButtons: Dialog.Ok

        Text {
            text: "新主题将在下次启动 Dream Machine 时生效。"
            wrapMode: Text.WordWrap
        }
    }
}