// src/default_plugin/qml/main.qml
// Dream Machine 默认框架主窗口
// 容器 ID（用于插件扩展）：
//   - sessionListContainer   : 会话列表区域
//   - chatViewContainer      : 对话流区域
//   - inputAreaContainer     : 输入区域（含输入框和发送按钮）
//   - statusBarContainer     : 状态栏区域

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 导入组件子目录，使 ChatView、SessionList 等类型可用
import "components"
// 导入卡片子目录（如 TracePopup 可能在其他地方使用）
// import "cards"

ApplicationWindow {
    id: rootWindow

    // ---------- 从全局参数读取配置 ----------
    readonly property var g: globalParams  // 由 C++ 注入

    width: g.layout.min_window_width
    height: g.layout.min_window_height
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
                }
            }
        }
    }
}