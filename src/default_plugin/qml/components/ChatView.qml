// src/default_plugin/qml/components/ChatView.qml
// 对话流显示组件
// 显示用户消息、AI 消息、系统消息，支持滚动、任务状态行和溯源链接

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    color: "transparent"

    // ---------- 属性 ----------
    property var chatModel: []                 // 对话消息列表
    property var onAnchorClicked: function(anchorId) {}  // 溯源链接点击回调
    property var onTaskStatusClicked: function(taskId) {} // 任务状态点击回调

    // ---------- 布局 ----------
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // 对话流列表
        ListView {
            id: listView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            model: root.chatModel
            delegate: messageDelegate

            // 滚动到最新消息
            onCountChanged: {
                if (count > 0) {
                    positionViewAtEnd()
                }
            }

            // 滚动条
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }

            // 空状态
            Text {
                anchors.centerIn: parent
                text: "开始对话吧！"
                color: root.g ? root.g.colors.text_muted : "#666666"
                font.pointSize: root.g ? root.g.fonts.size_large : 14
                visible: listView.count === 0
            }
        }
    }

    // ---------- 消息条目委托 ----------
    Component {
        id: messageDelegate

        Column {
            width: listView.width - 16
            spacing: 2
            leftPadding: 8
            rightPadding: 8

            // 根据消息类型渲染不同样式
            Loader {
                width: parent.width
                sourceComponent: {
                    if (modelData.type === "user") {
                        return userMessageComponent
                    } else if (modelData.type === "ai") {
                        return aiMessageComponent
                    } else if (modelData.type === "system") {
                        return systemMessageComponent
                    } else if (modelData.type === "task") {
                        return taskMessageComponent
                    } else {
                        return defaultMessageComponent
                    }
                }
            }
        }
    }

    // ----- 用户消息 -----
    Component {
        id: userMessageComponent

        Rectangle {
            width: parent.width
            height: userText.height + 16
            color: root.g ? root.g.colors.primary : "#3A7BD5"
            radius: root.g ? root.g.spacing.border_radius : 4

            Text {
                id: userText
                anchors.fill: parent
                anchors.margins: 8
                text: modelData.content || ""
                color: "#FFFFFF"
                font.pointSize: root.g ? root.g.fonts.size_normal : 12
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignRight
            }
        }
    }

    // ----- AI 消息 -----
    Component {
        id: aiMessageComponent

        Rectangle {
            width: parent.width
            height: aiText.height + 16
            color: root.g ? root.g.colors.surface_alt : "#2A2A3E"
            radius: root.g ? root.g.spacing.border_radius : 4

            Text {
                id: aiText
                anchors.fill: parent
                anchors.margins: 8
                text: modelData.content || ""
                color: root.g ? root.g.colors.text_primary : "#EEEEEE"
                font.pointSize: root.g ? root.g.fonts.size_normal : 12
                wrapMode: Text.Wrap
                onLinkActivated: {
                    // 处理溯源链接 [src:#5A]
                    if (root.onAnchorClicked) {
                        root.onAnchorClicked(link)
                    }
                }
            }
        }
    }

    // ----- 系统消息（浅色小字） -----
    Component {
        id: systemMessageComponent

        Rectangle {
            width: parent.width
            height: systemText.height + 8
            color: "transparent"

            Text {
                id: systemText
                anchors.fill: parent
                anchors.margins: 4
                text: "🔄 " + (modelData.content || "")
                color: root.g ? root.g.colors.text_secondary : "#AAAAAA"
                font.pointSize: root.g ? root.g.fonts.size_small : 10
                wrapMode: Text.Wrap
                opacity: 0.8
            }
        }
    }

    // ----- 任务状态消息（支持点击展开） -----
    Component {
        id: taskMessageComponent

        Rectangle {
            id: taskRect
            width: parent.width
            height: taskRow.height + 16
            color: modelData.status === "error" ?
                (root.g ? root.g.colors.error : "#F44336") :
                (modelData.status === "done" ?
                    (root.g ? root.g.colors.success : "#4CAF50") :
                    "transparent")
            radius: root.g ? root.g.spacing.border_radius : 4
            border.color: modelData.status === "running" ?
                (root.g ? root.g.colors.primary : "#3A7BD5") :
                "transparent"
            border.width: modelData.status === "running" ? 1 : 0

            RowLayout {
                id: taskRow
                anchors.fill: parent
                anchors.margins: 8
                spacing: 8

                // 状态图标
                Text {
                    text: {
                        if (modelData.status === "running") return "🔄"
                        if (modelData.status === "done") return "✅"
                        if (modelData.status === "error") return "❌"
                        return "📋"
                    }
                    font.pointSize: root.g ? root.g.fonts.size_normal : 12
                    Layout.preferredWidth: 24
                }

                // 任务描述
                Text {
                    text: modelData.content || ""
                    color: {
                        if (modelData.status === "error") return "#FFFFFF"
                        if (modelData.status === "done") return "#FFFFFF"
                        return root.g ? root.g.colors.text_primary : "#EEEEEE"
                    }
                    font.pointSize: root.g ? root.g.fonts.size_small : 10
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                }

                // 点击区域（点击弹出 TracePopup）
                MouseArea {
                    anchors.fill: parent
                    enabled: modelData.taskId !== undefined
                    onClicked: {
                        if (root.onTaskStatusClicked) {
                            root.onTaskStatusClicked(modelData.taskId)
                        }
                    }
                }
            }
        }
    }

    // ----- 默认消息（fallback） -----
    Component {
        id: defaultMessageComponent

        Rectangle {
            width: parent.width
            height: defaultText.height + 16
            color: root.g ? root.g.colors.surface_alt : "#2A2A3E"
            radius: root.g ? root.g.spacing.border_radius : 4

            Text {
                id: defaultText
                anchors.fill: parent
                anchors.margins: 8
                text: modelData.content || ""
                color: root.g ? root.g.colors.text_primary : "#EEEEEE"
                font.pointSize: root.g ? root.g.fonts.size_normal : 12
                wrapMode: Text.Wrap
            }
        }
    }

    // ---------- 将全局参数传递到内部 ----------
    property var g: globalParams || null

    // ---------- 便捷方法：添加消息 ----------
    function addMessage(type, content, status, taskId) {
        var msg = {
            type: type,
            content: content
        }
        if (status !== undefined) {
            msg.status = status
        }
        if (taskId !== undefined) {
            msg.taskId = taskId
        }
        chatModel.push(msg)
        // 滚动到底部
        listView.positionViewAtEnd()
        return chatModel.length - 1
    }

    // ---------- 便捷方法：更新任务状态 ----------
    function updateTask(index, status, content) {
        if (index < 0 || index >= chatModel.length) {
            return
        }
        if (chatModel[index].type !== "task") {
            return
        }
        if (status !== undefined) {
            chatModel[index].status = status
        }
        if (content !== undefined) {
            chatModel[index].content = content
        }
        chatModel = chatModel  // 触发刷新
    }

    // ---------- 便捷方法：清空 ----------
    function clear() {
        chatModel = []
    }
}