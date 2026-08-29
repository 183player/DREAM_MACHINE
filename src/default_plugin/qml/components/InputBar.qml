// src/default_plugin/qml/components/InputBar.qml
// 输入栏组件（输入框 + 发送按钮）
// 容器 ID：inputAreaContainer（用于插件扩展）

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    color: "transparent"

    // ---------- 属性 ----------
    property string placeholderText: "输入消息..."
    property alias text: inputArea.text
    property bool sendEnabled: inputArea.text.trim().length > 0
    property var onSend: function(text) {}    // 发送消息回调
    property var onTyping: function(text) {}  // 输入中回调

    // ---------- 布局 ----------
    RowLayout {
        anchors.fill: parent
        spacing: 8

        // 输入框（多行）
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: root.g ? root.g.colors.background : "#1E1E2E"
            radius: root.g ? root.g.spacing.border_radius : 4
            border.color: inputArea.focus ?
                (root.g ? root.g.colors.primary : "#3A7BD5") :
                (root.g ? root.g.colors.border : "#3A3A5A")
            border.width: 1

            ScrollView {
                anchors.fill: parent
                anchors.margins: 2
                clip: true
                ScrollBar.vertical.policy: ScrollBar.AsNeeded

                TextArea {
                    id: inputArea
                    width: parent.width
                    height: Math.min(120, Math.max(40, implicitHeight))
                    placeholderText: root.placeholderText
                    color: root.g ? root.g.colors.text_primary : "#EEEEEE"
                    font.pointSize: root.g ? root.g.fonts.size_normal : 12
                    wrapMode: TextArea.Wrap
                    background: Rectangle { color: "transparent" }

                    // 输入变化时触发回调
                    onTextChanged: {
                        if (root.onTyping) {
                            root.onTyping(text)
                        }
                    }

                    // Ctrl+Enter 发送
                    Keys.onPressed: {
                        if (event.key === Qt.Key_Enter || event.key === Qt.Key_Return) {
                            if (event.modifiers & Qt.ControlModifier) {
                                event.accepted = true
                                sendMessage()
                            }
                        }
                    }
                }
            }
        }

        // 发送按钮
        Button {
            id: sendButton
            Layout.preferredWidth: 60
            Layout.fillHeight: true
            enabled: root.sendEnabled
            text: "发送"
            onClicked: {
                sendMessage()
            }

            // 样式
            background: Rectangle {
                color: sendButton.enabled ?
                    (root.g ? root.g.colors.primary : "#3A7BD5") :
                    (root.g ? root.g.colors.text_muted : "#666666")
                radius: root.g ? root.g.spacing.border_radius : 4
            }
            contentItem: Text {
                text: sendButton.text
                color: sendButton.enabled ? "#FFFFFF" :
                    (root.g ? root.g.colors.text_secondary : "#AAAAAA")
                font.pointSize: root.g ? root.g.fonts.size_normal : 12
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
    }

    // ---------- 内部函数 ----------
    function sendMessage() {
        var msg = inputArea.text.trim()
        if (msg.length === 0) {
            return
        }
        if (root.onSend) {
            root.onSend(msg)
        }
        inputArea.text = ""
        inputArea.focus = true
    }

    // ---------- 快捷键 ----------
    // Enter 发送（非 Ctrl+Enter）
    Connections {
        target: inputArea
        function onKeyPressed(event) {
            if (event.key === Qt.Key_Enter || event.key === Qt.Key_Return) {
                if (!(event.modifiers & Qt.ControlModifier)) {
                    event.accepted = true
                    sendMessage()
                }
            }
        }
    }

    // ---------- 将全局参数传递到内部 ----------
    property var g: globalParams || null
}